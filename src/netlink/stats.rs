//! Network interface statistics via /proc/net/dev.

/// Pure parser function to parse a single line from `/proc/net/dev`.
/// Format: `  <iface>: <rx_bytes> <rx_packets> ... <tx_bytes> ...`
/// Returns `Some((ifname, rx_bytes, tx_bytes))` on success.
pub fn parse_proc_net_dev_line(line: &str) -> Option<(&str, u64, u64)> {
    let (name_part, stats_part) = line.split_once(':')?;
    let name = name_part.trim();
    if name.is_empty() {
        return None;
    }

    let mut tokens = stats_part.split_ascii_whitespace();
    let rx_bytes = tokens.next()?.parse::<u64>().ok()?;
    for _ in 0..7 {
        tokens.next()?;
    }
    let tx_bytes = tokens.next()?.parse::<u64>().ok()?;

    Some((name, rx_bytes, tx_bytes))
}
