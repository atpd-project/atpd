//! Version reporting module for ATPD.

pub fn get_full_version() -> &'static str {
    env!("ATPD_FULL_VERSION")
}

pub fn get_commit() -> &'static str {
    env!("ATPD_COMMIT")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_version_ported() {
        let full = get_full_version();
        let commit = get_commit();

        assert!(!full.is_empty(), "full version must not be empty");
        assert!(!commit.is_empty(), "commit must not be empty");
    }
}
