use std::io;

/// Returns field 22 (starttime) of a /proc/<pid>/stat line. comm (field 2) may contain spaces
/// and ')', so fields are counted from the last ')'.
pub fn parse_stat_starttime(stat: &str) -> Option<u64> {
    let mut fields = stat[stat.rfind(')')? + 1..].split_ascii_whitespace();
    // Field 3 (state) is a single character; fields 4-21 must be numeric (some may be negative).
    if fields.next()?.len() != 1 {
        return None;
    }
    for _ in 4..22 {
        fields.next()?.parse::<i64>().ok()?;
    }
    fields.next()?.parse().ok()
}

pub fn process_starttime(pid: i32) -> io::Result<u64> {
    if pid <= 0 {
        return Err(io::Error::from_raw_os_error(libc::EINVAL));
    }
    // comm is arbitrary bytes, so don't require the whole line to be UTF-8.
    let content = std::fs::read(format!("/proc/{}/stat", pid))?;
    parse_stat_starttime(&String::from_utf8_lossy(&content)).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "failed to parse /proc stat starttime",
        )
    })
}
