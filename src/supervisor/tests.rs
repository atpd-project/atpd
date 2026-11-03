use super::*;

// Mirrors tests/test_service_credentials.c: root credentials resolution without lookups
#[test]
fn test_credentials_root() {
    let mut passwd_lookups = 0;
    let mut group_lookups = 0;

    let creds = service_credentials_resolve_with_lookups(
        Some("root"),
        Some("root"),
        123,
        456,
        |_| {
            passwd_lookups += 1;
            None
        },
        |_| {
            group_lookups += 1;
            None
        },
    )
    .expect("resolve root credentials");

    assert_eq!(creds.uid, 0);
    assert_eq!(creds.gid, 0);
    assert!(!creds.init_groups);
    assert_eq!(passwd_lookups, 0);
    assert_eq!(group_lookups, 0);
}

// Mirrors tests/test_service_credentials.c: numeric uid and gid resolution
#[test]
fn test_credentials_numeric() {
    let creds = service_credentials_resolve_with_lookups(
        Some("1234"),
        Some("5678"),
        0,
        0,
        |_| None,
        |_| None,
    )
    .expect("resolve numeric credentials");

    assert_eq!(creds.uid, 1234);
    assert_eq!(creds.gid, 5678);
    assert!(!creds.init_groups);
}

// Mirrors tests/test_service_credentials.c: named user and group resolution on host system
#[test]
fn test_credentials_named_host() {
    let creds = service_credentials_resolve(Some("nobody"), Some("nogroup"), 0, 0)
        .expect("resolve nobody/nogroup");

    assert_eq!(creds.uid, 65534);
    assert_eq!(creds.gid, 65534);
    assert!(creds.init_groups);
    assert_eq!(creds.user_name, "nobody");
}

// Mirrors tests/test_service_credentials.c: non-existent user lookup failure
#[test]
fn test_credentials_nonexistent_user() {
    let mut passwd_lookups = 0;
    let mut group_lookups = 0;

    let res = service_credentials_resolve_with_lookups(
        Some("atpd-user-that-must-not-exist"),
        Some("0"),
        0,
        0,
        |_| {
            passwd_lookups += 1;
            None
        },
        |_| {
            group_lookups += 1;
            None
        },
    );

    assert!(res.is_err());
    assert_eq!(passwd_lookups, 1);
    assert_eq!(group_lookups, 0);
}

// Covers default values when user or group are empty or None
#[test]
fn test_credentials_defaults() {
    let creds1 =
        service_credentials_resolve_with_lookups(None, None, 1000, 2000, |_| None, |_| None)
            .expect("resolve with None");

    assert_eq!(creds1.uid, 1000);
    assert_eq!(creds1.gid, 2000);
    assert!(!creds1.init_groups);

    let creds2 = service_credentials_resolve_with_lookups(
        Some(""),
        Some(""),
        1000,
        2000,
        |_| None,
        |_| None,
    )
    .expect("resolve with empty strings");

    assert_eq!(creds2.uid, 1000);
    assert_eq!(creds2.gid, 2000);
    assert!(!creds2.init_groups);
}

// Covers username overlong (>= 64 chars) rejection with ENAMETOOLONG
#[test]
fn test_credentials_overlong_username() {
    let long_user = "a".repeat(64);
    let res = service_credentials_resolve_with_lookups(
        Some("valid_input"),
        None,
        0,
        0,
        |_| Some((1000, 1000, long_user.clone())),
        |_| None,
    );

    assert!(res.is_err());
    let err = res.unwrap_err();
    assert_eq!(err.raw_os_error(), Some(libc::ENAMETOOLONG));
}

// Covers invalid numeric IDs (non-digit characters) rejection with EINVAL
#[test]
fn test_credentials_invalid_numeric() {
    let res_user =
        service_credentials_resolve_with_lookups(Some("123bad"), None, 0, 0, |_| None, |_| None);
    assert!(res_user.is_err());
    assert_eq!(res_user.unwrap_err().raw_os_error(), Some(libc::EINVAL));

    let res_group =
        service_credentials_resolve_with_lookups(None, Some("456bad"), 0, 0, |_| None, |_| None);
    assert!(res_group.is_err());
    assert_eq!(res_group.unwrap_err().raw_os_error(), Some(libc::EINVAL));
}
