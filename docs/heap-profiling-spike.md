# Experimental refill sampling and secondary samples

The implementation is gated by independent build defines:

- `SNMALLOC_PROFILE_REFILL_SAMPLING`
- `SNMALLOC_PROFILE_SECONDARY_STORAGE`
- `SNMALLOC_PROFILE_REFILL_SECONDARY` (enables both)

Every experimental define requires `SNMALLOC_PROFILE`. With all experimental
defines off, the existing per-allocation sampler and profile-slot storage are
unchanged.

## Comparing a live HEAP profile with tcmalloc

Build snmalloc with `SNMALLOC_PROFILE` only: leave
`SNMALLOC_PROFILE_REFILL_SAMPLING`,
`SNMALLOC_PROFILE_SECONDARY_STORAGE`, and
`SNMALLOC_PROFILE_REFILL_SECONDARY` disabled. That comparable path advances
one Poisson countdown with each allocation's requested bytes. Refill sampling
advances by sizeclass bytes and therefore does not represent the same byte
stream.

Run the same workload under each allocator and record both sampling
intervals. snmalloc defaults to 512 KiB, while tcmalloc commonly uses a larger
interval (about 2 MiB). Compare weighted live bytes in the pprof
`inuse_space` view, not raw sample counts:

```sh
go tool pprof -sample_index=inuse_space snmalloc-heap.pb
go tool pprof -sample_index=space tcmalloc-heap.pb
```

The snmalloc profile records `period_type = ("space", "bytes")` and the
active interval. Its live-space estimate uses the same requested-byte
identity as tcmalloc:
`weight * allocated_size / (requested_size + 1)`.
Stack capture skips the sampler's own frame. In symbolicated pprof output,
leading `snmalloc::`, `snmalloc_rs::`, `snmalloc_sys::`, and `sn_rust_`
frames are then removed so the leaf is the application's allocation site.

## Alloc: trim the freelist at refill

The refill sampler has one Poisson byte countdown per small sizeclass and a
separate stream for large slow-path allocations. It does not use a global
refill budget. A refill transfers only the ordinary objects before the next
sample. The following allocation reaches `small_refill`, where the combined
variant returns one secondary sample and redraws that class's countdown.
Independent rate-`1 / sampling_rate` processes superpose, so the expected
aggregate sample count remains total allocated sizeclass bytes divided by the
configured sampling rate. This is intentionally not requested-byte sampling:
at refill time the allocator knows the sizeclass of future objects but not
their eventual requested sizes. Workloads with requests substantially below
their sizeclass capacity therefore have a different sampling identity from the
current requested-byte profiler.

`freelist::Builder::close_prefix` detaches a signed prefix and retains all
surplus in the builder. This works with the two-list `random_preserve` builder.
Full close resets tracked length for both randomized and ordinary builders;
otherwise a later append would inherit a stale count and could make slab
`needed_` reach unused prematurely.
`FrontendSlabMetadata::alloc_free_list` reports the exact number transferred,
including the object immediately returned to the current allocation; that
exact count is the sampler debit.

The per-allocation `on_alloc` countdown compiles away in refill mode.

In the refill-only cell, a fired node remains pending while the current
main-heap refill object is obtained. The allocator then fills in its address,
sizes, and timestamp, installs the existing `ProfileSlot`, and broadcasts it.
Only ordinary objects transferred behind that sampled return object debit the
newly drawn interval.

Pending main-heap samples survive lazy allocator initialization. Backend
allocation failure cancels and recycles the pending node. During profiler
reentrancy, one ordinary object may pass while a due countdown remains
negative; the next non-reentrant refill retries the sample and preserves the
overshoot in its weight.
Prepared nodes are not inserted into the global live list until their actual
main-heap or secondary address and size fields are complete, so concurrent
snapshots cannot observe a zero-address pending record.

An OOM after a refill decision consumes that experimental sample interval.
The pending node is cancelled without leaking or carrying stale state into the
next allocation; restoring the exact pre-draw Poisson state would require
rollback of sampler PRNG and countdown state.

## Free: put samples in the secondary allocator

`ProfileSecondaryAllocator` reserves mappings directly from the OS. Mappings
remain registered for process lifetime and inactive blocks are reused; they
are never concurrently unmapped. Exact-base ownership, capacity, and free are
looked up under a small allocator-local spin lock. No operation recursively
calls snmalloc.

Secondary configurations use `NoClientMetaDataProvider`, so ordinary owned
frees do not inspect profile slots and alloc/dealloc hooks compile away.
Sample nodes still use the existing process-global `SampledList` and
`NodePool`, including requested/allocated size, interval, weight, thread,
timestamp, and stack.

`SecondaryAllocatorChain` dispatches `deallocate` and `alloc_size` by exact
ownership. GWP-ASan is first when both are enabled. Secondary allocations
always take realloc's allocate-copy-free path, even when the requested size
would otherwise remain in the same sizeclass. `calloc` still zeroes through
the normal continuation after the secondary allocation succeeds.

The secondary-only cell retains the current per-allocation countdown but
prepares its decision before allocation, allowing a fired small fast-path
request to be routed to secondary storage without recording a main-heap
allocation first.

## Local comparison

Measured on an arm64 Mac16,8 with 24 GiB RAM, macOS 25.5, and AppleClang 21.
These are machine-local results, not pinned-Linux numbers. Release Criterion
runs used a fresh process per cell, a 512 KiB active sampling interval, and 50
measurements. Times below are the center estimate for a batch of 64
allocations and frees.

| Design | 32 B hold | 4 KiB hold | mixed hold | 32 B immediate free |
| --- | ---: | ---: | ---: | ---: |
| profiling compiled out | 0.800 us | 1.600 us | 1.538 us | 0.812 us |
| current countdown + main slots | 0.854 us | 1.613 us | 1.514 us | 0.814 us |
| refill + main slots | 0.834 us | 1.670 us | 1.527 us | 0.832 us |
| countdown + secondary | 0.828 us | 1.703 us | 1.490 us | 0.808 us |
| refill + secondary | 0.840 us | 1.658 us | 1.467 us | 0.828 us |

Against the current profiler, the combined design was 1.7% faster for 32 B
hold, 2.8% slower for 4 KiB hold, 3.2% faster for mixed hold, and 1.8% slower
for immediate free. This run therefore shows throughput parity within about
3%, not a broad speedup.

The disassembly result is stronger: after outlining `small_refill`, the
combined 32 B list-pop allocation path is instruction-for-instruction the
same as profiling compiled out. The current profiler adds a TLS countdown
load, subtract, store, and branch after every list pop. On owned free, the
combined design removes the current profile-slot lookup; tracking prefix
length adds one counter increment before the normal slab counter decrement.
Secondary ownership lookup remains only on the unowned-pointer path.

At an intentionally extreme sampling rate of one byte, holding 2,048 sampled
32/64 B allocations raised peak RSS from 8.3 MB for current main-heap slots to
41.6 MB for the secondary design. The experimental allocator keeps one
page-rounded mapping per concurrently live sampled block and retains mappings
for reuse, so memory is its clear current weakness.

All four cells pass sample/drain tests. The combined cell passes mixed-class
accuracy, realloc copy-out, chain dispatch, the five-second eight-thread
stress test, and the same stress test under ASan. TSan reports races in the
shared `NodePool`/snapshot machinery in both the current and combined
profilers; that pre-existing issue is not resolved by this spike.

## Verdict

The two suggestions work together and remove profiling work from the hottest
allocation and owned-free paths. The local throughput result is neutral, not
a measurable overall win. I would keep the refill design, but I would not
replace the current profiler with this secondary allocator yet: it needs
denser arena storage, and the shared profiler races need a separate fix.
