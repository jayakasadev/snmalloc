#![cfg(feature = "profiling")]

use snmalloc_rs::profile::default_output_path;
use std::env;
use std::path::PathBuf;

const ENV_OUT: &str = "SNMALLOC_PROFILE_OUT";
const ENV_BAZEL: &str = "TEST_UNDECLARED_OUTPUTS_DIR";

struct EnvGuard {
    key: &'static str,
    prior: Option<String>,
}

impl EnvGuard {
    fn save(key: &'static str) -> Self {
        let prior = env::var(key).ok();
        Self { key, prior }
    }
}

impl Drop for EnvGuard {
    fn drop(&mut self) {
        match &self.prior {
            Some(v) => env::set_var(self.key, v),
            None => env::remove_var(self.key),
        }
    }
}

#[test]
fn precedence_chain_exhaustive() {
    let _g_out = EnvGuard::save(ENV_OUT);
    let _g_bazel = EnvGuard::save(ENV_BAZEL);

    env::set_var(ENV_OUT, "/tmp/explicit.folded");
    env::set_var(ENV_BAZEL, "/tmp/bazel_should_be_ignored");
    let p = default_output_path();
    assert_eq!(
        p,
        PathBuf::from("/tmp/explicit.folded"),
        "SNMALLOC_PROFILE_OUT must take precedence verbatim"
    );

    env::set_var(ENV_OUT, "");
    env::set_var(ENV_BAZEL, "/tmp/bazel_outputs");
    let p = default_output_path();
    assert_eq!(
        p,
        PathBuf::from("/tmp/bazel_outputs/heap.folded"),
        "empty SNMALLOC_PROFILE_OUT must fall through to Bazel path"
    );

    env::remove_var(ENV_OUT);
    env::set_var(ENV_BAZEL, "/tmp/bazel_outputs");
    let p = default_output_path();
    assert_eq!(
        p,
        PathBuf::from("/tmp/bazel_outputs/heap.folded"),
        "TEST_UNDECLARED_OUTPUTS_DIR must be suffixed with heap.folded"
    );

    env::remove_var(ENV_OUT);
    env::remove_var(ENV_BAZEL);
    let p = default_output_path();
    let tmp = env::temp_dir();
    assert!(
        p.starts_with(&tmp),
        "fallback path {p:?} must live under temp_dir {tmp:?}"
    );
    let fname = p
        .file_name()
        .expect("fallback path has a file name")
        .to_str()
        .expect("file name is valid utf-8");
    let expected = format!("heap_{}.folded", std::process::id());
    assert_eq!(fname, expected, "fallback file name must encode the PID");
}
