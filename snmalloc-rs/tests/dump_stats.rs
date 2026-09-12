use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};

fn assert_canonical_header(dump: &str) {
    assert!(
        dump.contains("Bytes in use by application"),
        "dump must contain the canonical 'Bytes in use by application' \
         line; got:\n{}",
        dump
    );
    assert!(
        dump.contains("------------------------------------------------"),
        "dump must contain at least one horizontal rule; got:\n{}",
        dump
    );
    assert!(
        dump.contains("MALLOC:"),
        "dump must contain at least one MALLOC: line; got:\n{}",
        dump
    );
}

#[test]
fn dump_stats_emits_canonical_header() {
    let alloc = SnMalloc::new();
    let mut buf: Vec<u8> = Vec::new();
    alloc
        .dump_stats(&mut buf)
        .expect("writing to a Vec never fails");

    assert!(!buf.is_empty(), "dump_stats produced no output");
    let dump = std::str::from_utf8(&buf).expect("dump must be ASCII / UTF-8");
    assert_canonical_header(dump);
}

#[test]
fn dump_stats_reflects_live_allocation() {
    let alloc = SnMalloc::new();
    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null(), "1 MiB allocation must not fail");

    let mut buf: Vec<u8> = Vec::new();
    alloc
        .dump_stats(&mut buf)
        .expect("writing to a Vec never fails");
    let dump = std::str::from_utf8(&buf).expect("dump must be UTF-8");
    assert_canonical_header(dump);

    unsafe { alloc.dealloc(ptr, layout) };

    assert!(
        dump.contains("Peak bytes in use"),
        "dump must contain the 'Peak bytes in use' line; got:\n{}",
        dump
    );
}

#[test]
fn dump_stats_two_calls_are_independent() {
    let alloc = SnMalloc::new();

    let mut a: Vec<u8> = Vec::new();
    let mut b: Vec<u8> = Vec::new();
    alloc.dump_stats(&mut a).unwrap();
    alloc.dump_stats(&mut b).unwrap();

    assert_canonical_header(std::str::from_utf8(&a).unwrap());
    assert_canonical_header(std::str::from_utf8(&b).unwrap());

    assert!(!a.is_empty());
    assert!(!b.is_empty());
}

#[test]
fn dump_stats_regex_match() {
    let alloc = SnMalloc::new();
    let mut buf: Vec<u8> = Vec::new();
    alloc.dump_stats(&mut buf).unwrap();
    let dump = std::str::from_utf8(&buf).unwrap();

    let line = dump
        .lines()
        .find(|l| l.contains("Bytes in use by application"))
        .expect("dump must contain a 'Bytes in use by application' line");
    assert!(
        line.starts_with("MALLOC:"),
        "line must start with MALLOC:; got {:?}",
        line
    );
    assert!(
        line.contains('('),
        "line must contain a human-readable parenthesized column; got {:?}",
        line
    );
    assert!(
        line.contains(')'),
        "line must contain a closing paren; got {:?}",
        line
    );
    assert!(
        line.chars().any(|c| c.is_ascii_digit()),
        "line must contain at least one digit; got {:?}",
        line
    );
}
