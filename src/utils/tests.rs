use super::proc::*;

// Mirrors tests/test_utils_proc_stat.c: 2. Negative PID check
#[test]
fn test_proc_negative_pid() {
    assert!(process_starttime(-1).is_err());
}

// Mirrors tests/test_utils_proc_stat.c: 3. Non-existent large PID check
#[test]
fn test_proc_nonexistent_pid() {
    assert!(process_starttime(99999999).is_err());
}

// Mirrors tests/test_utils_proc_stat.c: 4. Valid self PID check & 5. Repeatability
#[test]
fn test_proc_self_pid_repeatability() {
    let pid = std::process::id() as i32;
    let start1 = process_starttime(pid).expect("read self starttime");
    assert!(start1 > 0);

    let start2 = process_starttime(pid).expect("read self starttime again");
    assert_eq!(start1, start2);
}

// Mirrors tests/test_utils_proc_stat.c: 6. System root / init PID 1 check
#[test]
fn test_proc_init_pid() {
    // PID 1 may or may not be accessible depending on container/namespace, but shouldn't panic
    let _ = process_starttime(1);
}

#[test]
fn test_parse_stat_starttime_comm_spaces_and_parens() {
    // Normal process stat line
    let stat_normal = "1234 (bash) S 1 2 3 4 -1 0 0 0 0 0 0 0 0 0 0 0 0 0 54321 0 0 0 0";
    assert_eq!(parse_stat_starttime(stat_normal), Some(54321));

    // Comm containing spaces
    let stat_spaces =
        "5678 (web worker pool 1) R 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 998877 20 21";
    assert_eq!(parse_stat_starttime(stat_spaces), Some(998877));

    // Comm containing nested parentheses
    let stat_parens =
        "9012 (proc (sub) [worker]) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 112233 20";
    assert_eq!(parse_stat_starttime(stat_parens), Some(112233));

    // Comm containing both parentheses and spaces
    let stat_complex =
        "1111 (my (special) process name) D 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 778899 20";
    assert_eq!(parse_stat_starttime(stat_complex), Some(778899));
}

#[test]
fn test_parse_stat_starttime_malformed() {
    // Missing closing parenthesis
    assert_eq!(parse_stat_starttime("1234 (unclosed comm"), None);

    // Fewer than 22 fields
    assert_eq!(parse_stat_starttime("1234 (proc) S 1 2 3 4 5"), None);

    // Invalid state character
    assert_eq!(
        parse_stat_starttime(
            "1234 (proc) INVALID 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 123 20"
        ),
        None
    );

    // Non-numeric field where number expected
    assert_eq!(
        parse_stat_starttime(
            "1234 (proc) S 1 2 3 4 5 6 7 8 notanumber 10 11 12 13 14 15 16 17 18 19 123 20"
        ),
        None
    );
}
