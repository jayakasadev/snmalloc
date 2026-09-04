use std::fs;

/// ```text
/// LazyFree:              0 kB
/// ```
pub fn read_smaps_rollup_field_kb(field: &str) -> u64 {
    let text = fs::read_to_string("/proc/self/smaps_rollup")
        .expect("failed to read /proc/self/smaps_rollup");
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix(field) {
            return rest
                .trim()
                .trim_end_matches("kB")
                .trim()
                .parse()
                .unwrap_or_else(|e| panic!("failed to parse {field} value from {rest:?}: {e}"));
        }
    }
    panic!("{field} line not found in /proc/self/smaps_rollup");
}

pub fn read_lazyfree_bytes() -> u64 {
    read_smaps_rollup_field_kb("LazyFree:") * 1024
}

pub fn read_rss_bytes() -> u64 {
    read_smaps_rollup_field_kb("Rss:") * 1024
}
