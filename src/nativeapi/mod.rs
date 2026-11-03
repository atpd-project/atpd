//! sing-box Native API client, telemetry dispatcher, and supervisor probe integration.
//!
//! Mirrors `src/singbox_api.c`, `include/singbox_api.h`, `src/api.c`, and `include/api.h`.

pub mod client;
pub mod proto;
pub mod status;

#[cfg(test)]
mod tests;

pub use client::{Client, ClientError};
pub use status::{ApiSnapshot, SingboxClashModeStatus, SingboxStatus};

use crate::config::Config;
use crate::context::{VpnChange, VpnState};
use crate::reactor::Waker;
use std::io;
use std::time::Duration;

pub const API_REFRESH_INTERVAL_MS: u64 = 1000;
pub const API_REFRESH_RPC_COUNT: u64 = 3;
pub const API_VPN_RPC_COUNT: u64 = 2;
pub const API_REFRESH_RPC_BUDGET_MS: u64 = 2000;
pub const API_SNAPSHOT_STALE_MARGIN_MS: u64 = 1000;

pub const API_SNAPSHOT_STALE_MS: u64 = API_REFRESH_INTERVAL_MS
    + (API_REFRESH_RPC_COUNT + API_VPN_RPC_COUNT) * API_REFRESH_RPC_BUDGET_MS
    + API_SNAPSHOT_STALE_MARGIN_MS;

#[derive(Debug, Clone, Default)]
struct ApiSnapshotCandidate {
    valid: bool,
    version_valid: bool,
    clash_mode_valid: bool,
    status: SingboxStatus,
    version: String,
    clash_mode: String,
    default_mode: Option<String>,
}

#[derive(Debug, Clone)]
enum WorkerRequest {
    Vpn(VpnRequest),
    Shutdown,
}

#[derive(Debug, Clone)]
struct VpnRequest {
    state: VpnState,
    target_mode: String,
    fallback_mode: String,
}

/// Dispatcher and controller handle owned by the reactor thread.
/// Mirrors `api_ctx_t` from `src/api.c`.
pub struct ApiHandle {
    rx: std::sync::mpsc::Receiver<ApiSnapshotCandidate>,
    tx: std::sync::mpsc::Sender<WorkerRequest>,
    #[allow(dead_code)]
    waker: Waker,
    config: Config,
    snapshot: ApiSnapshot,
    default_mode: Option<String>,
    worker: Option<std::thread::JoinHandle<()>>,
}

impl ApiHandle {
    /// Initialize API controller and spawn the dedicated network refresh worker thread.
    /// Signals are blocked around the spawn so the worker does not intercept reactor signals.
    pub fn new(config: &Config, waker: Waker) -> io::Result<Self> {
        let client = Client::from_config(config)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
        Self::new_with_client(config, client, waker)
    }

    /// Initialize API controller with an explicit `Client` instance.
    pub fn new_with_client(config: &Config, client: Client, waker: Waker) -> io::Result<Self> {
        let (tx_candidate, rx_candidate) = std::sync::mpsc::channel();
        let (tx_request, rx_request) = std::sync::mpsc::channel();

        let worker_handle = spawn_worker(rx_request, tx_candidate, waker.clone(), client)?;

        Ok(Self {
            rx: rx_candidate,
            tx: tx_request,
            waker,
            config: config.clone(),
            snapshot: ApiSnapshot::default(),
            default_mode: None,
            worker: Some(worker_handle),
        })
    }

    pub fn config(&self) -> &Config {
        &self.config
    }

    pub fn default_mode(&self) -> Option<&str> {
        self.default_mode.as_deref()
    }

    /// Drain incoming telemetry candidates delivered by the refresh worker.
    /// Called on the reactor thread when `Event::Wakeup` is received.
    /// Mirrors `api_result_cb` from `src/api.c`.
    pub fn on_wakeup(&mut self) {
        let mut latest = None;
        while let Ok(candidate) = self.rx.try_recv() {
            latest = Some(candidate);
        }

        if let Some(candidate) = latest {
            self.snapshot.generation = self.snapshot.generation.wrapping_add(1);
            self.snapshot.updated_at_ms = crate::reactor::now_ms();
            self.snapshot.valid = candidate.valid;
            self.snapshot.version_valid = candidate.version_valid;
            self.snapshot.clash_mode_valid = candidate.clash_mode_valid;
            self.snapshot.status = candidate.status;
            self.snapshot.version = candidate.version;
            self.snapshot.clash_mode = candidate.clash_mode;
            self.default_mode = candidate.default_mode;
        }
    }

    /// Copy-out of the latest telemetry snapshot with bounded staleness check.
    /// No locking is performed on the read path.
    /// Mirrors `api_get_snapshot` from `src/api.c`.
    pub fn snapshot(&self) -> ApiSnapshot {
        let mut out = self.snapshot.clone();
        let now = crate::reactor::now_ms();
        if out.valid
            && (now < out.updated_at_ms
                || now.saturating_sub(out.updated_at_ms) > API_SNAPSHOT_STALE_MS)
        {
            out.valid = false;
            out.version_valid = false;
            out.clash_mode_valid = false;
        }
        out
    }

    /// Receive VPN state transitions from ATPD context and dispatch mode updates.
    /// Mirrors `api_vpn_mode_callback` from `src/api.c`.
    pub fn on_vpn_change(&mut self, change: VpnChange) {
        if !self.config.interface.vpn_auto_mode {
            return;
        }

        let target_state = if change.state == VpnState::Ready {
            VpnState::Ready
        } else if change.state == VpnState::Idle
            || change.state == VpnState::Teardown
            || change.teardown
        {
            VpnState::Idle
        } else {
            return;
        };

        let target_mode = if !self.config.interface.vpn_target_mode.is_empty() {
            self.config.interface.vpn_target_mode.clone()
        } else {
            "Google VPN".to_string()
        };

        let fallback_mode = if !self.config.interface.vpn_fallback_mode.is_empty() {
            self.config.interface.vpn_fallback_mode.clone()
        } else {
            "Rule".to_string()
        };

        let req = VpnRequest {
            state: target_state,
            target_mode,
            fallback_mode,
        };

        let _ = self.tx.send(WorkerRequest::Vpn(req));
    }

    /// Shutdown the refresh worker thread.
    /// Mirrors `api_cleanup` from `src/api.c`.
    pub fn shutdown(&mut self) {
        if let Some(handle) = self.worker.take() {
            let _ = self.tx.send(WorkerRequest::Shutdown);

            // Plain join like api_cleanup: the worker's HTTP calls are bounded by their own timeouts.
            if handle.join().is_err() {
                crate::log_warn!("API worker thread panicked");
            }
        }
    }

    #[cfg(test)]
    pub fn set_snapshot_for_test(&mut self, snap: ApiSnapshot) {
        self.snapshot = snap;
    }
}

impl Drop for ApiHandle {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// Small probes struct implementing `crate::supervisor::Probes`.
/// - `api_ready`: runs a status probe against sing-box with 1s timeout.
/// - `api_healthy`: checks snapshot validity and freshness.
pub struct ApiProbes<'a> {
    client: Client,
    api: &'a ApiHandle,
    start_time_ms: u64,
}

impl<'a> ApiProbes<'a> {
    pub fn new(api: &'a ApiHandle) -> Result<Self, ClientError> {
        let client = Client::new(
            &api.config().api.host,
            api.config().api.port as u16,
            &api.config().api.secret,
            Duration::from_secs(1),
        )?;
        Ok(Self {
            client,
            api,
            start_time_ms: 0,
        })
    }

    pub fn from_client(client: Client, api: &'a ApiHandle) -> Self {
        Self {
            client,
            api,
            start_time_ms: 0,
        }
    }

    pub fn with_start_time(mut self, start_time_ms: u64) -> Self {
        self.start_time_ms = start_time_ms;
        self
    }
}

impl<'a> crate::supervisor::Probes for ApiProbes<'a> {
    /// Probe native API readiness via SubscribeStatus with 1s timeout.
    /// Mirrors `service_probe_native_api` in `src/service.c`.
    fn api_ready(&mut self) -> bool {
        self.client.get_status().is_ok()
    }

    /// Check service health against latest published snapshot.
    /// Mirrors `api_get_snapshot` check in `src/service.c`.
    fn api_healthy(&mut self) -> bool {
        let snap = self.api.snapshot();
        snap.valid && (self.start_time_ms == 0 || snap.updated_at_ms >= self.start_time_ms)
    }
}

/// Case-insensitive match against Clash mode list.
/// Mirrors `match_clash_mode` in `src/api.c`.
pub fn match_clash_mode<'a>(status: &'a SingboxClashModeStatus, mode: &str) -> Option<&'a str> {
    if mode.is_empty() {
        return None;
    }
    for m in &status.modes {
        if m.eq_ignore_ascii_case(mode) {
            return Some(m.as_str());
        }
    }
    None
}

/// Check if a Clash mode is supported.
/// Mirrors `is_clash_mode_supported` in `src/api.c`.
pub fn is_clash_mode_supported(status: &SingboxClashModeStatus, mode: &str) -> bool {
    match_clash_mode(status, mode).is_some()
}

fn spawn_worker(
    rx: std::sync::mpsc::Receiver<WorkerRequest>,
    tx: std::sync::mpsc::Sender<ApiSnapshotCandidate>,
    waker: Waker,
    client: Client,
) -> io::Result<std::thread::JoinHandle<()>> {
    let mut all_signals = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
    // SAFETY: sigfillset fills all_signals to block all signals in worker thread.
    let ret = unsafe { libc::sigfillset(all_signals.as_mut_ptr()) };
    if ret != 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: all_signals is initialized by sigfillset.
    let all_signals = unsafe { all_signals.assume_init() };

    let mut prev_signals = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
    // SAFETY: pthread_sigmask sets thread mask to block all signals and captures previous mask.
    let ret = unsafe {
        libc::pthread_sigmask(libc::SIG_SETMASK, &all_signals, prev_signals.as_mut_ptr())
    };
    if ret != 0 {
        return Err(io::Error::from_raw_os_error(ret));
    }
    // SAFETY: prev_signals is initialized by pthread_sigmask.
    let prev_signals = unsafe { prev_signals.assume_init() };

    let spawn_res = std::thread::Builder::new()
        .name("atpd-api-worker".to_string())
        .spawn(move || {
            worker_loop(rx, tx, waker, client);
        });

    // SAFETY: restoring previous signal mask in current thread immediately after spawn.
    let restore_ret =
        unsafe { libc::pthread_sigmask(libc::SIG_SETMASK, &prev_signals, std::ptr::null_mut()) };

    match spawn_res {
        Ok(handle) => {
            if restore_ret != 0 {
                crate::log_warn!(
                    "Failed to restore reactor thread signal mask after worker spawn: {}",
                    restore_ret
                );
            }
            Ok(handle)
        }
        Err(e) => {
            if restore_ret != 0 {
                crate::log_warn!(
                    "Failed to restore reactor thread signal mask on spawn failure: {}",
                    restore_ret
                );
            }
            Err(e)
        }
    }
}

fn worker_loop(
    rx: std::sync::mpsc::Receiver<WorkerRequest>,
    tx: std::sync::mpsc::Sender<ApiSnapshotCandidate>,
    waker: Waker,
    client: Client,
) {
    let mut default_mode: Option<String> = None;
    let mut pending_vpn: Option<VpnRequest> = None;
    let mut last_err = String::new();

    loop {
        // Drain incoming requests, keeping the latest VPN request
        while let Ok(msg) = rx.try_recv() {
            match msg {
                WorkerRequest::Shutdown => return,
                WorkerRequest::Vpn(req) => pending_vpn = Some(req),
            }
        }

        // Apply VPN request if pending. If it fails, keep it in pending_vpn to retry next round.
        if let Some(req) = pending_vpn.as_ref() {
            let ok = apply_vpn_mode(&client, &mut default_mode, req);
            if ok {
                pending_vpn = None;
            }
        }

        // Refresh telemetry candidate
        let mut err = String::new();
        let mut candidate = refresh_candidate(&client, &mut err);
        // Log each distinct failure once so a broken API shows up in atp.log without spam.
        if err != last_err {
            if !err.is_empty() {
                crate::log_warn!("Native API: refresh failed: {}", err);
            }
            last_err = err;
        }
        candidate.default_mode = default_mode.clone();

        // Send candidate to reactor and wake epoll loop
        if tx.send(candidate).is_err() {
            return;
        }
        let _ = waker.wake();

        // Sleep between refreshes with API_REFRESH_INTERVAL_MS, wake immediately on request
        match rx.recv_timeout(Duration::from_millis(API_REFRESH_INTERVAL_MS)) {
            Ok(WorkerRequest::Shutdown) => return,
            Ok(WorkerRequest::Vpn(req)) => pending_vpn = Some(req),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return,
        }
    }
}

fn refresh_candidate(client: &Client, err: &mut String) -> ApiSnapshotCandidate {
    let mut next = ApiSnapshotCandidate::default();
    match client.get_status() {
        Ok(status) => {
            next.valid = true;
            next.status = status;
        }
        Err(e) => {
            *err = format!("SubscribeStatus: {}", e);
            return next;
        }
    }

    match client.get_version() {
        Ok(ver) if !ver.is_empty() => {
            next.version_valid = true;
            next.version = ver;
        }
        Ok(_) => *err = "GetVersion: empty version".to_string(),
        Err(e) => *err = format!("GetVersion: {}", e),
    }

    match client.get_clash_mode_status() {
        Ok(mode_status) if !mode_status.current_mode.is_empty() => {
            next.clash_mode_valid = true;
            next.clash_mode = mode_status.current_mode;
        }
        Ok(_) => *err = "GetClashModeStatus: empty current mode".to_string(),
        Err(e) => *err = format!("GetClashModeStatus: {}", e),
    }
    next
}

fn apply_vpn_mode(
    client: &Client,
    default_mode: &mut Option<String>,
    request: &VpnRequest,
) -> bool {
    let status = match client.get_clash_mode_status() {
        Ok(s) => s,
        Err(_) => {
            crate::log_warn!("Native API: Clash mode service unavailable during VPN sync");
            return false;
        }
    };

    if request.state == VpnState::Idle {
        let saved = match default_mode.as_ref() {
            Some(m) if !m.is_empty() => m.clone(),
            _ => return true,
        };

        let mut restore_mode = saved.as_str();
        if !is_clash_mode_supported(&status, restore_mode) {
            if !is_clash_mode_supported(&status, &request.fallback_mode) {
                crate::log_warn!(
                    "Native API: neither saved Clash mode '{}' nor fallback '{}' is available",
                    restore_mode,
                    request.fallback_mode
                );
                return false;
            }
            restore_mode = &request.fallback_mode;
        }

        if let Some(matched) = match_clash_mode(&status, restore_mode) {
            restore_mode = matched;
        }

        if status.current_mode != restore_mode {
            if let Err(e) = client.set_clash_mode(restore_mode) {
                crate::log_warn!(
                    "Native API: failed to restore Clash mode to {}: {:?}",
                    restore_mode,
                    e
                );
                return false;
            }
        }

        crate::log_info!(
            "Native API: VPN state IDLE restored Clash mode {}",
            restore_mode
        );
        *default_mode = None;
        return true;
    }

    // State is Ready
    if default_mode.is_none() {
        *default_mode = Some(status.current_mode.clone());
    }

    if !is_clash_mode_supported(&status, &request.target_mode) {
        crate::log_warn!(
            "Native API: Target Clash mode '{}' is not present in sing-box mode list",
            request.target_mode
        );
        return false;
    }

    let target_match =
        match_clash_mode(&status, &request.target_mode).unwrap_or(&request.target_mode);

    if status.current_mode == target_match {
        return true;
    }

    if let Err(e) = client.set_clash_mode(target_match) {
        crate::log_warn!(
            "Native API: failed to switch Clash mode to {}: {:?}",
            target_match,
            e
        );
        return false;
    }

    crate::log_info!(
        "Native API: VPN state READY selected Clash mode '{}' (restore={})",
        target_match,
        default_mode.as_deref().unwrap_or(&request.fallback_mode)
    );
    true
}
