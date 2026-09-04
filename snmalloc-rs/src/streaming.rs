//! Safe streaming-profile FFI wrapper.
//!
//! [`StreamSample`] borrows callback data. [`ProfilingSession`] owns
//! registration and unregisters on drop. Only one session may run;
//! another [`ProfilingSession::start`] returns
//! [`StreamingError::AlreadyActive`].
//!
//! This module requires the `profiling` feature.

extern crate alloc;
extern crate std;

use alloc::boxed::Box;
use core::ffi::c_void;
use core::fmt;
use core::marker::PhantomData;
use core::slice;

use std::sync::{Mutex, OnceLock};

use snmalloc_sys as ffi;
use snmalloc_sys::SnRustProfileRawSample;

/// Streaming event kind.
///
/// - [`EventKind::Alloc`]: new sampled allocation.
/// - [`EventKind::Resize`]: in-place realloc. [`StreamSample`] contains
///  new sizes; its stack and Poisson weight stay unchanged.
///
/// Out-of-place realloc emits allocation and deallocation events, not
/// `Resize`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum EventKind {
    /// A fresh sampled allocation.
    Alloc,
    /// An in-place realloc updated an existing sample's size.
    Resize,
}

impl EventKind {
    /// Decode [`SnRustProfileRawSample::kind`].
    ///
    /// Unknown values become [`EventKind::Alloc`].
    #[inline]
    fn from_raw(kind: u8) -> Self {
        match kind {
            snmalloc_sys::SN_RUST_PROFILE_KIND_RESIZE => EventKind::Resize,
            _ => EventKind::Alloc,
        }
    }
}

/// Callback for each streaming event.
///
/// Events may arrive on any allocator thread. A [`Mutex`] serializes
/// calls. The C registration requires a `'static` closure.
type Handler = Box<dyn Fn(StreamSample<'_>) + Send + Sync + 'static>;

/// Process-global handler slot. `None` means no session is active.
/// The outer `OnceLock` is initialised lazily on first
/// [`ProfilingSession::start`]; the inner `Mutex` enforces
/// single-session-at-a-time semantics and provides safe shared
/// access between the registering thread and the (possibly many)
/// allocator threads dispatching through the trampoline.
fn handler_slot() -> &'static Mutex<Option<Handler>> {
    static SLOT: OnceLock<Mutex<Option<Handler>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

/// Borrowed view of a single streaming sample.
///
/// The lifetime parameter ties the view to the duration of the C
/// callback dispatch. The user closure receives `StreamSample<'_>`
/// by value, and the borrow check prevents the closure from stashing
/// any field that aliases the raw sample buffer — the C side reuses
/// that stack-allocated buffer across broadcasts.
///
/// All accessors return values, not references, so the user can
/// freely copy out individual fields if they need to keep them past
/// the callback (e.g. by cloning the stack into a `Vec`).
///
/// # Example
///
/// Print the per-sample fields from inside a streaming session:
///
/// ```no_run
/// use snmalloc_rs::ProfilingSession;
///
/// let _session = ProfilingSession::start(|sample| {
///     eprintln!(
///         "sampled {:p} requested={} allocated={} weight={} depth={}",
///         sample.alloc_ptr(),
///         sample.requested_size(),
///         sample.allocated_size(),
///         sample.weight(),
///         sample.stack().len(),
///     );
///
///     // Frames are borrowed -- copy them out if you need to keep
///     // the stack past this callback invocation.
///     let owned_stack: Vec<*const core::ffi::c_void> = sample.stack().to_vec();
///     let _ = owned_stack;
/// }).expect("session should start");
/// ```
#[derive(Copy, Clone)]
pub struct StreamSample<'a> {
    raw: &'a SnRustProfileRawSample,
    _phantom: PhantomData<&'a ()>,
}

impl<'a> StreamSample<'a> {
    /// SAFETY: the caller must ensure `raw` is valid for `'a` and
    /// the entire `SnRustProfileRawSample` (including the inline
    /// stack array) has been initialised by the C side.
    #[inline]
    unsafe fn from_raw(raw: &'a SnRustProfileRawSample) -> Self {
        Self {
            raw,
            _phantom: PhantomData,
        }
    }

    /// Pointer returned to the application by the original
    /// allocation. Opaque — intended only for debugging / cross-
    /// referencing with application-side bookkeeping. May be null
    /// in pathological corner cases.
    #[inline]
    pub fn alloc_ptr(&self) -> *const c_void {
        self.raw.alloc_ptr as *const c_void
    }

    /// Bytes the original caller requested.
    #[inline]
    pub fn requested_size(&self) -> usize {
        self.raw.requested_size
    }

    /// Bytes returned by snmalloc (sizeclass-rounded).
    #[inline]
    pub fn allocated_size(&self) -> usize {
        self.raw.allocated_size
    }

    /// Bytes-of-request Poisson weight for this sample. Summing
    /// across the broadcast stream gives an unbiased estimator of
    /// total bytes requested.
    #[inline]
    pub fn weight(&self) -> u64 {
        self.raw.weight as u64
    }

    /// Event kind tag for this broadcast. See [`EventKind`] for the
    /// semantic distinction between an alloc-time broadcast
    /// ([`EventKind::Alloc`]) and an in-place realloc resize-event
    /// broadcast ([`EventKind::Resize`]).
    ///
    /// Consumers that care about live-bytes attribution per call site
    /// should treat a `Resize` event as updating the latest known
    /// `requested_size` / `allocated_size` for the original alloc;
    /// consumers that only count distinct allocations can filter
    /// `kind() == Alloc` to recover pre-Resize semantics.
    #[inline]
    pub fn kind(&self) -> EventKind {
        EventKind::from_raw(self.raw.kind)
    }

    /// Captured return addresses, innermost first. Slice length is
    /// `stack_depth`. Borrowed from the raw sample for the
    /// duration of the callback; if the user needs to keep the
    /// frames past the callback they must copy them out (e.g. with
    /// `to_vec()`).
    #[inline]
    pub fn stack(&self) -> &[*const c_void] {
        let depth = self.raw.stack_depth as usize;
        let max = snmalloc_sys::SN_RUST_PROFILE_STACK_FRAMES;
        let n = if depth <= max { depth } else { max };
        // SAFETY: `raw.stack` is a fixed-size array of `*mut c_void`
        // initialised by the C side; we narrow to `n` entries which
        // is bounded by the array length. `*mut c_void` and
        // `*const c_void` have identical layout so the reinterpret
        // is sound.
        unsafe { slice::from_raw_parts(self.raw.stack.as_ptr() as *const *const c_void, n) }
    }
}

impl<'a> fmt::Debug for StreamSample<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("StreamSample")
            .field("alloc_ptr", &self.alloc_ptr())
            .field("requested_size", &self.requested_size())
            .field("allocated_size", &self.allocated_size())
            .field("weight", &self.weight())
            .field("stack_depth", &self.stack().len())
            .field("kind", &self.kind())
            .finish()
    }
}

/// Reasons [`ProfilingSession::start`] can fail.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StreamingError {
    /// A session is already active in this process. Drop it before
    /// starting a new one.
    AlreadyActive,
    /// The C-side registration failed (e.g. profiling not supported
    /// at build time, or all broadcast slots are taken by C++-side
    /// subscribers).
    RegistrationFailed,
}

impl fmt::Display for StreamingError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StreamingError::AlreadyActive => {
                f.write_str("a snmalloc profiling streaming session is already active")
            }
            StreamingError::RegistrationFailed => {
                f.write_str("failed to register the snmalloc streaming callback with the C runtime")
            }
        }
    }
}

impl std::error::Error for StreamingError {}

/// Fixed `extern "C"` trampoline registered with the C side. Every
/// sampled allocation funnels through here, regardless of which
/// Rust closure the user supplied. The trampoline locks the global
/// handler slot, dispatches into the stored closure (if any), and
/// returns — the lock window is the duration of the user closure.
///
/// The slot is read under a `Mutex` for safety; the C contract
/// requires the trampoline to be reentrancy-free w.r.t. allocator
/// activity (the allocator may sample during the user closure on
/// another thread but never on this thread mid-dispatch), and the
/// `Mutex` is held only for the brief callback dispatch.
unsafe extern "C" fn trampoline(sample: *const SnRustProfileRawSample) {
    if sample.is_null() {
        return;
    }

    let raw = &*sample;
    let view = StreamSample::from_raw(raw);

    let guard = match handler_slot().lock() {
        Ok(g) => g,
        Err(_) => return,
    };
    if let Some(handler) = guard.as_ref() {
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            handler(view);
        }));
        let _ = result;
    }
}

/// RAII handle for an active streaming-profiling session.
///
/// Construct one via [`ProfilingSession::start`]. While the handle
/// is alive, the closure receives each [`StreamSample`]. Dropping the
/// handle unregisters the closure
/// from the C runtime and clears the global handler slot, freeing
/// up the next [`ProfilingSession::start`] to succeed.
///
/// Only one session can be active per process; a second
/// [`ProfilingSession::start`] while one is already alive returns
/// [`StreamingError::AlreadyActive`].
///
/// The type is `!Send` and `!Sync` (via the `*const ()`
/// phantom): dropping the session must happen on a single thread,
/// not across thread boundaries, so the unregister-then-clear
/// sequence inside `Drop` is well-ordered.
pub struct ProfilingSession {
    _not_send: PhantomData<*const ()>,
}

impl ProfilingSession {
    /// Begin a streaming profiling session.
    ///
    /// `handler` receives each event on its allocator thread. Its
    /// borrowed [`StreamSample`] is valid only during the call.
    ///
    /// # Errors
    ///
    /// - [`StreamingError::AlreadyActive`] — another
    ///  `ProfilingSession` is alive in this process.
    /// - [`StreamingError::RegistrationFailed`] — the C runtime
    ///  refused to register the trampoline (most commonly because
    ///  `SNMALLOC_PROFILE` is disabled at build time, or every
    ///  broadcast slot is already claimed).
    ///
    /// # Example
    ///
    /// Count the sampled allocations into a shared atomic, then tear
    /// down the session by dropping the returned handle:
    ///
    /// ```no_run
    /// use snmalloc_rs::{ProfilingSession, SnMalloc};
    /// use std::sync::Arc;
    /// use std::sync::atomic::{AtomicU64, Ordering};
    ///
    /// let allocator = SnMalloc::new();
    /// allocator.set_sampling_rate(65_536);
    ///
    /// let count = Arc::new(AtomicU64::new(0));
    /// let count_for_handler = Arc::clone(&count);
    /// let session = ProfilingSession::start(move |sample| {
    ///     count_for_handler.fetch_add(sample.weight(), Ordering::Relaxed);
    /// }).expect("session should start");
    ///
    /// // ... run the workload ...
    ///
    /// drop(session); // unregisters the handler; another session can start now.
    /// println!("total sampled weight: {}", count.load(Ordering::Relaxed));
    /// ```
    pub fn start<F>(handler: F) -> Result<Self, StreamingError>
    where
        F: Fn(StreamSample<'_>) + Send + Sync + 'static,
    {
        let mut guard = match handler_slot().lock() {
            Ok(g) => g,
            Err(poisoned) => poisoned.into_inner(),
        };
        if guard.is_some() {
            return Err(StreamingError::AlreadyActive);
        }

        *guard = Some(Box::new(handler));

        // SAFETY: `trampoline` is a fixed-signature C-compatible
        // function pointer that survives for the lifetime of the
        // process; the C side stores it in a `std::atomic`. We
        // hold the slot mutex across the registration so no other
        // start() can interleave between the slot write and the
        // C-side store.
        let rc = unsafe { ffi::sn_rust_profile_streaming_start(trampoline) };
        if rc != 0 {
            *guard = None;
            return Err(StreamingError::RegistrationFailed);
        }

        drop(guard);

        Ok(Self {
            _not_send: PhantomData,
        })
    }
}

impl fmt::Debug for ProfilingSession {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ProfilingSession").finish_non_exhaustive()
    }
}

impl Drop for ProfilingSession {
    fn drop(&mut self) {
        unsafe {
            let _ = ffi::sn_rust_profile_streaming_stop();
        }

        if let Ok(mut guard) = handler_slot().lock() {
            *guard = None;
        }
    }
}
