//! sing-box Native API status and telemetry data structures.

pub const SINGBOX_MAX_CLASH_MODES: usize = 32;

/// Snapshot returned by StartedService.SubscribeStatus.
/// Mirrors `daemon.Status` in sing-box and C `singbox_status_t`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct SingboxStatus {
    pub memory: u64,
    pub goroutines: i32,
    pub connections_in: i32,
    pub connections_out: i32,
    pub traffic_available: bool,
    pub uplink: i64,
    pub downlink: i64,
    pub uplink_total: i64,
    pub downlink_total: i64,
}

/// Status returned by StartedService.GetClashModeStatus.
/// Mirrors `daemon.ClashModeStatus` and C `singbox_clash_mode_status_t`.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SingboxClashModeStatus {
    pub modes: Vec<String>,
    pub current_mode: String,
}

/// Reactor-visible API snapshot delivered by the worker.
/// Mirrors C `api_snapshot_t`.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ApiSnapshot {
    pub valid: bool,
    pub version_valid: bool,
    pub clash_mode_valid: bool,
    pub generation: u64,
    pub updated_at_ms: u64,
    pub status: SingboxStatus,
    pub version: String,
    pub clash_mode: String,
}
