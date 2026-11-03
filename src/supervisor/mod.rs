pub mod credentials;
pub mod health;
pub mod process;

#[cfg(test)]
mod tests;

use crate::config::Config;
use crate::reactor::{Reactor, TimerId};
use std::io;
use std::path::Path;

/// SIGTERM/SIGINT are blocked for the reactor's signalfd, so while the reactor
/// isn't running (startup wait) they sit in the pending set.
fn stop_signal_pending() -> bool {
    let mut set = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
    // SAFETY: sigpending fills the set; it is only read after a successful call.
    if unsafe { libc::sigpending(set.as_mut_ptr()) } != 0 {
        return false;
    }
    // SAFETY: set was initialized by sigpending above.
    let set = unsafe { set.assume_init() };
    // SAFETY: sigismember only reads the initialized set.
    unsafe {
        libc::sigismember(&set, libc::SIGTERM) == 1 || libc::sigismember(&set, libc::SIGINT) == 1
    }
}

pub trait Probes {
    fn api_ready(&mut self) -> bool;
    fn api_healthy(&mut self) -> bool;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServiceState {
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Failed = 3,
    Stopping = 4,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ServiceSnapshot {
    pub child_pid: Option<i32>,
    pub state: ServiceState,
    pub healthy: bool,
    pub start_time: u64,
}

#[derive(Debug)]
pub enum SupervisorError {
    Io(std::io::Error),
    CircuitOpen,
    BinaryNotFound(String),
    ConfigValidationFailed(String),
    SpawnFailed(String),
}

impl std::fmt::Display for SupervisorError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SupervisorError::Io(e) => write!(f, "I/O error: {}", e),
            SupervisorError::CircuitOpen => write!(f, "circuit breaker is open"),
            SupervisorError::BinaryNotFound(path) => {
                write!(f, "service binary not found: {}", path)
            }
            SupervisorError::ConfigValidationFailed(reason) => {
                write!(f, "config validation failed: {}", reason)
            }
            SupervisorError::SpawnFailed(reason) => write!(f, "spawn failed: {}", reason),
        }
    }
}

impl std::error::Error for SupervisorError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            SupervisorError::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<std::io::Error> for SupervisorError {
    fn from(e: std::io::Error) -> Self {
        SupervisorError::Io(e)
    }
}

pub struct Supervisor {
    pub bin_path: String,
    pub work_dir: String,
    pub conf_path: String,
    pub log_path: String,
    pub pid_path: String,
    pub user: String,
    pub group: String,
    pub service_args: String,
    pub service_env: String,
    pub api_port: i32,
    pub child_pid: Option<i32>,
    pub child_starttime_ticks: u64,
    pub validated_pid: bool,
    pub state: ServiceState,
    pub fail_count: i32,
    pub max_failures: i32,
    pub start_timeout_sec: i32,
    pub stop_timeout_sec: i32,
    pub health_check_interval_ms: i32,
    pub monitor_timer: Option<TimerId>,
    pub retry_timer: Option<TimerId>,
    pub health_timer: Option<TimerId>,
    pub backoff: health::Backoff,
    pub breaker: health::CircuitBreaker,
    pub running_healthy: bool,
    pub last_health_check: u64,
    pub start_time: u64,
    pub start_time_ms: u64,
}

impl Supervisor {
    pub fn new(cfg: &Config) -> Result<Self, SupervisorError> {
        let base_dir = if cfg.core.data_dir.is_empty() {
            "."
        } else {
            &cfg.core.data_dir
        };

        let work_dir = base_dir.to_string();
        let bin_path = format!("{}/bin/sing-box", base_dir);
        let conf_path = format!("{}/config.json", base_dir);
        let log_path = format!("{}/sing-box.log", base_dir);

        let run_dir = if cfg.core.run_dir.is_empty() {
            crate::config::ATP_RUN_DIR
        } else {
            &cfg.core.run_dir
        };

        let pid_path = if run_dir.starts_with('/') {
            format!("{}/sing-box.pid", run_dir)
        } else {
            format!("{}/{}/sing-box.pid", base_dir, run_dir)
        };

        let max_failures = if cfg.service.max_failures > 0 {
            cfg.service.max_failures
        } else {
            5
        };

        let start_timeout_sec = if cfg.service.start_timeout_sec > 0 {
            cfg.service.start_timeout_sec
        } else {
            30
        };

        let stop_timeout_sec = if cfg.service.stop_timeout_sec > 0 {
            cfg.service.stop_timeout_sec
        } else {
            10
        };

        let health_check_interval_ms = if cfg.service.health_check_interval_ms > 0 {
            cfg.service.health_check_interval_ms
        } else {
            5000
        };

        let threshold = if cfg.service.circuit_threshold > 0 {
            cfg.service.circuit_threshold as u32
        } else {
            5
        };

        let cooldown = if cfg.service.circuit_cooldown_sec > 0 {
            cfg.service.circuit_cooldown_sec as u64
        } else {
            60
        };

        Ok(Self {
            bin_path,
            work_dir,
            conf_path,
            log_path,
            pid_path,
            user: cfg.core.core_user.clone(),
            group: cfg.core.core_group.clone(),
            service_args: cfg.service.args.clone(),
            service_env: cfg.service.env.clone(),
            api_port: cfg.api.port,
            child_pid: None,
            child_starttime_ticks: 0,
            validated_pid: false,
            state: ServiceState::Stopped,
            fail_count: 0,
            max_failures,
            start_timeout_sec,
            stop_timeout_sec,
            health_check_interval_ms,
            monitor_timer: None,
            retry_timer: None,
            health_timer: None,
            backoff: health::Backoff::new(),
            breaker: health::CircuitBreaker::new(threshold, cooldown),
            running_healthy: false,
            last_health_check: 0,
            start_time: 0,
            start_time_ms: 0,
        })
    }

    pub fn apply_config(&mut self, r: &mut Reactor, cfg: &Config) {
        let interval_ms = if cfg.service.health_check_interval_ms > 0 {
            cfg.service.health_check_interval_ms
        } else {
            5000
        };
        // Like service_apply_config: a new interval takes effect now, not at the next restart.
        if interval_ms != self.health_check_interval_ms && self.state == ServiceState::Running {
            if let Some(old) = self.health_timer.take() {
                r.cancel_timer(old);
            }
            self.health_timer = Some(r.add_timer(interval_ms as u64, interval_ms as u64));
        }
        self.max_failures = if cfg.service.max_failures > 0 {
            cfg.service.max_failures
        } else {
            5
        };
        self.start_timeout_sec = if cfg.service.start_timeout_sec > 0 {
            cfg.service.start_timeout_sec
        } else {
            30
        };
        self.stop_timeout_sec = if cfg.service.stop_timeout_sec > 0 {
            cfg.service.stop_timeout_sec
        } else {
            10
        };
        self.health_check_interval_ms = if cfg.service.health_check_interval_ms > 0 {
            cfg.service.health_check_interval_ms
        } else {
            5000
        };
    }

    pub fn validate_config(&self) -> Result<(), SupervisorError> {
        process::service_validate_config(
            &self.bin_path,
            &self.work_dir,
            &self.conf_path,
            &self.service_args,
        )
    }

    pub fn start(&mut self, r: &mut Reactor) -> Result<(), SupervisorError> {
        if self.state == ServiceState::Running || self.state == ServiceState::Starting {
            return Ok(());
        }

        if !self.breaker.should_allow() {
            return Err(SupervisorError::CircuitOpen);
        }

        if self.is_alive() {
            self.state = ServiceState::Starting;
            return Ok(());
        }

        if self.monitor_timer.is_none() {
            self.monitor_timer = Some(r.add_timer(100, 1000));
        }

        match self.spawn_process() {
            Ok((pid, starttime)) => {
                self.child_pid = Some(pid);
                self.child_starttime_ticks = starttime;
                self.validated_pid = true;
                self.state = ServiceState::Starting;
                self.start_time = health::system_now_sec();
                self.start_time_ms = crate::reactor::now_ms();
                self.fail_count = 0;
                self.backoff.reset();
                Ok(())
            }
            Err(e) => {
                self.state = ServiceState::Failed;
                self.breaker.record_failure();
                Err(e)
            }
        }
    }

    pub fn wait_ready(
        &mut self,
        r: &mut Reactor,
        probes: &mut dyn Probes,
    ) -> Result<(), SupervisorError> {
        let timeout_ms = (if self.start_timeout_sec > 0 {
            self.start_timeout_sec as u64
        } else {
            30
        }) * 1000;
        let deadline = crate::reactor::now_ms() + timeout_ms;
        while crate::reactor::now_ms() < deadline {
            self.handle_monitor_timer(r, probes);
            if self.is_running() {
                return Ok(());
            }
            if self.state == ServiceState::Failed || self.child_pid.is_none() {
                return Err(SupervisorError::SpawnFailed(
                    "service failed during startup readiness wait".to_string(),
                ));
            }
            if stop_signal_pending() {
                crate::log_info!("Service: stop requested during startup");
                return Err(SupervisorError::SpawnFailed(
                    "stop requested during startup".to_string(),
                ));
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        self.handle_monitor_timer(r, probes);
        if self.is_running() {
            Ok(())
        } else {
            Err(SupervisorError::SpawnFailed(
                "service timed out waiting for readiness".to_string(),
            ))
        }
    }

    pub fn stop(&mut self, r: &mut Reactor) -> Result<(), SupervisorError> {
        self.state = ServiceState::Stopping;

        if let Some(timer) = self.monitor_timer.take() {
            r.cancel_timer(timer);
        }
        if let Some(timer) = self.retry_timer.take() {
            r.cancel_timer(timer);
        }
        if let Some(timer) = self.health_timer.take() {
            r.cancel_timer(timer);
        }

        let mut result = Ok(());

        if let Some(pid) = self.child_pid.take() {
            crate::log_info!("Service: stopping sing-box (PID: {})", pid);
            let mut reaped = false;
            let attempts = if self.stop_timeout_sec > 0 {
                self.stop_timeout_sec * 10
            } else {
                10
            };

            let target = rustix::process::Pid::from_raw(pid);
            if let Some(target_pid) = target {
                if let Err(err) =
                    rustix::process::kill_process(target_pid, rustix::process::Signal::TERM)
                {
                    if err != rustix::io::Errno::SRCH {
                        result = Err(SupervisorError::Io(io::Error::from(err)));
                    }
                }
            } else {
                result = Err(SupervisorError::Io(io::Error::from(
                    rustix::io::Errno::SRCH,
                )));
            }

            for _ in 0..attempts {
                match rustix::process::waitpid(target, rustix::process::WaitOptions::NOHANG) {
                    Ok(Some((reaped_pid, _status))) => {
                        if target == Some(reaped_pid) {
                            reaped = true;
                            break;
                        }
                    }
                    Ok(None) => {}
                    Err(rustix::io::Errno::CHILD) => {
                        reaped = true;
                        break;
                    }
                    Err(rustix::io::Errno::INTR) => {}
                    Err(err) => {
                        result = Err(SupervisorError::Io(io::Error::from(err)));
                        break;
                    }
                }
                std::thread::sleep(std::time::Duration::from_millis(100));
            }

            if !reaped {
                crate::log_warn!(
                    "Service: sing-box did not exit within {}s, sending SIGKILL",
                    attempts / 10
                );
                if let Some(target_pid) = target {
                    if let Err(err) =
                        rustix::process::kill_process(target_pid, rustix::process::Signal::KILL)
                    {
                        if err != rustix::io::Errno::SRCH {
                            result = Err(SupervisorError::Io(io::Error::from(err)));
                        }
                    }
                } else {
                    result = Err(SupervisorError::Io(io::Error::from(
                        rustix::io::Errno::SRCH,
                    )));
                }
                while let Err(rustix::io::Errno::INTR) =
                    rustix::process::waitpid(target, rustix::process::WaitOptions::empty())
                {
                }
            }
        }

        self.child_pid = None;
        self.child_starttime_ticks = 0;
        self.validated_pid = false;
        self.running_healthy = false;
        self.state = ServiceState::Stopped;
        process::unlink_pid_file(Path::new(&self.pid_path));

        result
    }

    pub fn snapshot(&self) -> ServiceSnapshot {
        let pid = self.child_pid.filter(|&p| {
            // Defeat PID reuse: only report if PID is alive with matching starttime.
            process::service_is_alive(p, self.child_starttime_ticks)
        });
        let healthy = pid.is_some() && self.running_healthy;
        ServiceSnapshot {
            child_pid: pid,
            state: self.state,
            healthy,
            start_time: self.start_time,
        }
    }

    pub fn on_sigchld(&mut self, r: &mut Reactor) {
        let mut reaped_our_child = false;

        if let Some(pid) = self.child_pid {
            let target = rustix::process::Pid::from_raw(pid);
            loop {
                match rustix::process::waitpid(target, rustix::process::WaitOptions::NOHANG) {
                    Ok(Some((reaped_pid, status))) => {
                        if target == Some(reaped_pid) {
                            reaped_our_child = true;
                            if let Some(code) = status.exit_status() {
                                crate::log_warn!("Service: sing-box exited with code {}", code);
                            } else if let Some(sig) = status.terminating_signal() {
                                crate::log_warn!("Service: sing-box killed by signal {}", sig);
                            }
                        }
                        break;
                    }
                    Ok(None) => break,
                    Err(rustix::io::Errno::CHILD) => {
                        reaped_our_child = true;
                        break;
                    }
                    Err(rustix::io::Errno::INTR) => continue,
                    Err(_) => break,
                }
            }
        }

        if reaped_our_child {
            self.child_pid = None;
            self.child_starttime_ticks = 0;
            self.validated_pid = false;
            self.running_healthy = false;
            process::unlink_pid_file(Path::new(&self.pid_path));

            if let Some(ht) = self.health_timer.take() {
                r.cancel_timer(ht);
            }

            if self.state == ServiceState::Running || self.state == ServiceState::Starting {
                self.record_failure(r);
            }
        }
    }

    pub fn on_timer(&mut self, r: &mut Reactor, id: TimerId, probes: &mut dyn Probes) -> bool {
        if Some(id) == self.monitor_timer {
            self.handle_monitor_timer(r, probes);
            true
        } else if Some(id) == self.retry_timer {
            self.retry_timer = None;
            self.handle_retry_timer(r);
            true
        } else if Some(id) == self.health_timer {
            self.handle_health_timer(r, probes);
            true
        } else {
            false
        }
    }

    fn handle_monitor_timer(&mut self, r: &mut Reactor, probes: &mut dyn Probes) {
        match self.state {
            ServiceState::Stopped | ServiceState::Stopping => {}
            ServiceState::Starting => {
                let timeout_ms = (if self.start_timeout_sec > 0 {
                    self.start_timeout_sec as u64
                } else {
                    30
                }) * 1000;
                let now = crate::reactor::now_ms();

                if self.start_time_ms > 0 && now.saturating_sub(self.start_time_ms) >= timeout_ms {
                    crate::log_warn!("Service: startup timed out after {}s", timeout_ms / 1000);
                    if let Some(pid) = self.child_pid {
                        if let Some(target) = rustix::process::Pid::from_raw(pid) {
                            let _ = rustix::process::kill_process(
                                target,
                                rustix::process::Signal::KILL,
                            );
                        }
                    }
                    self.record_failure(r);
                    return;
                }

                if self.starting_child_alive() {
                    if let Some(pid) = self.child_pid {
                        if !self.validated_pid {
                            if process::validate_process(
                                pid,
                                self.child_starttime_ticks,
                                &self.bin_path,
                            ) {
                                self.validated_pid = true;
                            } else {
                                crate::log_warn!("Service: PID {} validation failed, killing", pid);
                                if let Some(target) = rustix::process::Pid::from_raw(pid) {
                                    let _ = rustix::process::kill_process(
                                        target,
                                        rustix::process::Signal::KILL,
                                    );
                                    while let Err(rustix::io::Errno::INTR) =
                                        rustix::process::waitpid(
                                            Some(target),
                                            rustix::process::WaitOptions::empty(),
                                        )
                                    {}
                                }
                                self.child_pid = None;
                                self.child_starttime_ticks = 0;
                                self.validated_pid = false;
                                process::unlink_pid_file(Path::new(&self.pid_path));
                                self.record_failure(r);
                                return;
                            }
                        }

                        if self.validated_pid && probes.api_ready() {
                            crate::log_info!("Service: Native API ready on port {}", self.api_port);
                            self.state = ServiceState::Running;
                            self.fail_count = 0;
                            self.running_healthy = true;
                            self.last_health_check = health::system_now_sec();
                            self.backoff.reset();
                            self.breaker.record_success();

                            if self.health_check_interval_ms > 0 {
                                if let Some(ht) = self.health_timer.take() {
                                    r.cancel_timer(ht);
                                }
                                let interval = self.health_check_interval_ms as u64;
                                self.health_timer = Some(r.add_timer(interval, interval));
                            }
                        }
                    }
                } else {
                    crate::log_warn!("Service: process died during startup");
                    self.record_failure(r);
                }
            }
            ServiceState::Running => {
                if !self.is_alive() {
                    crate::log_warn!("Service: process died unexpectedly");
                    self.record_failure(r);
                }
            }
            ServiceState::Failed => {
                if self.child_pid.is_none()
                    && self.retry_timer.is_none()
                    && self.breaker.should_allow()
                {
                    crate::log_info!("Service: cooldown expired, retrying");
                    self.fail_count = 0;
                    self.backoff.reset();
                    self.schedule_retry(r);
                }
            }
        }
    }

    fn starting_child_alive(&mut self) -> bool {
        let pid = match self.child_pid {
            Some(p) => p,
            None => return false,
        };
        if !process::service_is_alive(pid, self.child_starttime_ticks) {
            return false;
        }

        let target = rustix::process::Pid::from_raw(pid);
        let mut waited_reaped = false;
        let mut child_still_running = false;
        loop {
            match rustix::process::waitpid(target, rustix::process::WaitOptions::NOHANG) {
                Ok(Some((reaped_pid, _status))) => {
                    if target == Some(reaped_pid) {
                        waited_reaped = true;
                    }
                    break;
                }
                Ok(None) => {
                    child_still_running = true;
                    break;
                }
                Err(rustix::io::Errno::CHILD) => {
                    waited_reaped = true;
                    break;
                }
                Err(rustix::io::Errno::INTR) => continue,
                Err(_) => break,
            }
        }

        if child_still_running {
            return true;
        }

        if waited_reaped {
            self.child_pid = None;
            self.child_starttime_ticks = 0;
            self.validated_pid = false;
            process::unlink_pid_file(Path::new(&self.pid_path));
        }

        false
    }

    fn handle_retry_timer(&mut self, r: &mut Reactor) {
        if self.state == ServiceState::Stopped {
            return;
        }

        if !self.breaker.should_allow() {
            crate::log_warn!("Circuit breaker: open, delaying retry");
            self.schedule_retry(r);
            return;
        }

        match self.spawn_process() {
            Ok((pid, starttime)) => {
                self.child_pid = Some(pid);
                self.child_starttime_ticks = starttime;
                self.validated_pid = true;
                self.state = ServiceState::Starting;
                self.start_time = health::system_now_sec();
                self.start_time_ms = crate::reactor::now_ms();
            }
            Err(_) => self.record_failure(r),
        }
    }

    fn handle_health_timer(&mut self, _r: &mut Reactor, probes: &mut dyn Probes) {
        if self.state != ServiceState::Running {
            return;
        }

        let pid = match self.child_pid {
            Some(p) => p,
            None => {
                self.running_healthy = false;
                return;
            }
        };

        if !process::service_is_alive(pid, self.child_starttime_ticks) {
            self.running_healthy = false;
            return;
        }

        process::service_rotate_log(Path::new(&self.log_path));

        let healthy = probes.api_healthy();
        if healthy != self.running_healthy {
            if healthy {
                crate::log_info!("Service: health check recovered");
            } else {
                crate::log_warn!("Service: health check failed");
            }
        }
        self.running_healthy = healthy;
        self.last_health_check = health::system_now_sec();
        if healthy {
            self.breaker.record_success();
        } else {
            self.breaker.record_failure();
        }
    }

    /// Count a failed start or crash, then retry or give up.
    fn record_failure(&mut self, r: &mut Reactor) {
        self.state = ServiceState::Failed;
        self.fail_count += 1;
        self.breaker.record_failure();
        if self.fail_count >= self.max_failures {
            crate::log_error!("Service: max failures reached, giving up");
            // Hold the breaker open so the monitor retries once its cooldown expires.
            self.breaker.circuit_open = true;
        } else if self.breaker.is_open() {
            crate::log_warn!("Circuit breaker: threshold reached, entering cooldown");
        } else {
            self.schedule_retry(r);
        }
    }

    fn schedule_retry(&mut self, r: &mut Reactor) {
        let delay = self.backoff.next_delay();
        crate::log_info!(
            "Service: retry in {}ms ({}/{})",
            delay,
            self.fail_count,
            self.max_failures
        );
        if let Some(old) = self.retry_timer.take() {
            r.cancel_timer(old);
        }
        self.retry_timer = Some(r.add_timer(delay, 0));
    }

    fn spawn_process(&mut self) -> Result<(i32, u64), SupervisorError> {
        let res = process::service_spawn(
            &self.bin_path,
            &self.work_dir,
            &self.conf_path,
            &self.log_path,
            &self.pid_path,
            &self.user,
            &self.group,
            &self.service_args,
            &self.service_env,
        );
        match &res {
            Ok((pid, _)) => crate::log_info!("Service: spawned sing-box (PID: {})", pid),
            Err(e) => crate::log_error!("Service: {}", e),
        }
        res
    }

    pub fn is_alive(&self) -> bool {
        if let Some(pid) = self.child_pid {
            process::service_is_alive(pid, self.child_starttime_ticks)
        } else {
            false
        }
    }

    pub fn is_running(&self) -> bool {
        self.state == ServiceState::Running && self.is_alive()
    }
}

impl Drop for Supervisor {
    fn drop(&mut self) {
        if let Some(pid) = self.child_pid.take() {
            if pid > 0 && pid != std::process::id() as i32 {
                if let Some(target) = rustix::process::Pid::from_raw(pid) {
                    let _ = rustix::process::kill_process(target, rustix::process::Signal::KILL);
                    while let Err(rustix::io::Errno::INTR) = rustix::process::waitpid(
                        Some(target),
                        rustix::process::WaitOptions::empty(),
                    ) {}
                }
            }
            process::unlink_pid_file(Path::new(&self.pid_path));
        }
    }
}
