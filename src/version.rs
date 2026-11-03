//! Version reporting module for ATPD.

pub fn get_version() -> &'static str {
    env!("ATPD_VERSION")
}

pub fn get_full_version() -> &'static str {
    env!("ATPD_FULL_VERSION")
}

pub fn get_commit() -> &'static str {
    env!("ATPD_COMMIT")
}

pub fn get_build_time() -> &'static str {
    env!("ATPD_BUILD_TIME")
}

// C-compatible naming aliases
#[allow(unused_imports)]
pub use get_commit as atp_get_commit;
#[allow(unused_imports)]
pub use get_full_version as atp_get_full_version;
#[allow(unused_imports)]
pub use get_version as atp_get_version;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_version_ported() {
        let ver = atp_get_version();
        let full = atp_get_full_version();
        let commit = atp_get_commit();
        let btime = get_build_time();

        assert!(!ver.is_empty(), "version must not be empty");
        assert!(
            full.starts_with(ver),
            "full version '{}' must start with base version '{}'",
            full,
            ver
        );
        assert!(!commit.is_empty(), "commit must not be empty");
        assert!(!btime.is_empty(), "build time must not be empty");
    }
}
