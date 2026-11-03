//! ATPD Application Coordinator and Reactor Event Loop.
//!
//! Mirrors `src/atpd_init.c` and daemon event loop from `src/main.c`.

use std::fs;
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::RawFd;
use std::path::{Path, PathBuf};

use crate::cli::{CliOptions, Verbosity};
use crate::config::{Config, ReloadChanges};
use crate::context::{AtpdContext, RuntimeState};
use crate::ipc::{IpcRequest, IpcServer};
use crate::logger::{self, LogLevel};
use crate::nativeapi::{ApiHandle, ApiProbes};
use crate::netlink::NetlinkWatcher;
use crate::reactor::{Event, Reactor};
use crate::supervisor::Supervisor;

pub const TOKEN_ROUTE: u64 = 1;
pub const TOKEN_XFRM: u64 = 2;
pub const TOKEN_IPC_LISTEN: u64 = 3;
pub const TOKEN_IPC_CLIENT_BASE: u64 = 100;

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct StartupNotifyMsg {
    pub status: u8,
    pub pid: libc::pid_t,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PidIdentity {
    pub pid: i32,
    pub starttime: u64,
}

pub struct App {
    pub config: Config,
    pub config_path: PathBuf,
    pub reactor: Reactor,
    pub context: AtpdContext,
    pub supervisor: Supervisor,
    pub netlink: NetlinkWatcher,
    pub api: ApiHandle,
    pub ipc: IpcServer,
    pub pid_file: PathBuf,
    pub pid_fd: RawFd,
    pub startup_notify_fd: Option<RawFd>,
    pub shutdown_requested: bool,
}

impl App {
    /// Initialize all daemon components in the exact order required by `atpd_init.c` and `main.c`.
    /// `atp_timezone_init` and all `reactor.watch_signal` calls occur before `ApiHandle::new`
    /// spawns the API worker thread.
    pub fn new(
        opts: &CliOptions,
        config: Config,
        config_path: PathBuf,
        pid_file: PathBuf,
        pid_fd: RawFd,
        startup_notify_fd: Option<RawFd>,
    ) -> Result<Self, io::Error> {
        // 1. Context initialization & state transition
        let mut context = AtpdContext::new();
        if context.init().is_err() {
            return Err(io::Error::other("Failed to initialize ATPD context"));
        }
        if context
            .runtime_state_transition(RuntimeState::Initializing)
            .is_err()
        {
            return Err(io::Error::other(
                "Failed to transition context to INITIALIZING",
            ));
        }

        // 2. Logger setup
        logger::logger_init();
        if !config.core.log_file.is_empty() {
            let log_path = if config.core.log_file.starts_with('/') {
                PathBuf::from(&config.core.log_file)
            } else {
                let data_dir = if !config.core.data_dir.is_empty() {
                    &config.core.data_dir
                } else {
                    "."
                };
                PathBuf::from(data_dir).join(&config.core.log_file)
            };
            if let Some(parent) = log_path.parent() {
                if !parent.as_os_str().is_empty() {
                    let _ = fs::create_dir_all(parent);
                }
            }
            logger::log_set_file(&log_path);
        }
        let level = match opts.verbosity {
            Verbosity::Verbose => LogLevel::Debug,
            Verbosity::Quiet => LogLevel::Error,
            Verbosity::Default => LogLevel::Info,
        };
        logger::log_set_level(level);
        if opts.no_color {
            logger::log_set_color(false);
        }

        // 3. Reactor creation & signal watchers BEFORE any thread is spawned
        let mut reactor = Reactor::new()?;
        let watched_signals = [
            libc::SIGTERM,
            libc::SIGINT,
            libc::SIGHUP,
            libc::SIGUSR1,
            libc::SIGCHLD,
        ];
        for sig in watched_signals {
            reactor.watch_signal(sig)?;
        }

        // 4. Netlink watcher initialization
        let netlink = NetlinkWatcher::new(&mut reactor, TOKEN_ROUTE, TOKEN_XFRM)?;

        // 5. Supervisor initialization
        let supervisor = Supervisor::new(&config)
            .map_err(|e| io::Error::other(format!("Failed to initialize supervisor: {}", e)))?;

        // 6. Native API handle initialization (spawns background worker thread)
        let api = ApiHandle::new(&config, reactor.waker())?;

        // 7. IPC command socket server initialization
        let socket_path = resolve_socket_path(&config);
        if let Some(parent) = socket_path.parent() {
            if !parent.as_os_str().is_empty() {
                let _ = fs::create_dir_all(parent);
            }
        }
        // SAFETY: geteuid is a safe query for the current process effective user ID.
        let allowed_uid = unsafe { libc::geteuid() };
        let ipc = IpcServer::new(
            &socket_path,
            allowed_uid,
            TOKEN_IPC_LISTEN,
            TOKEN_IPC_CLIENT_BASE,
            &mut reactor,
        )?;

        Ok(Self {
            config,
            config_path,
            reactor,
            context,
            supervisor,
            netlink,
            api,
            ipc,
            pid_file,
            pid_fd,
            startup_notify_fd,
            shutdown_requested: false,
        })
    }

    /// Notify daemonizing parent of startup outcome over the notification pipe.
    pub fn notify_startup(&mut self, result: i32) {
        if let Some(fd) = self.startup_notify_fd.take() {
            // SAFETY: getpid is a reentrant syscall.
            let my_pid = unsafe { libc::getpid() };
            let msg = StartupNotifyMsg {
                status: if result == 0 { 0 } else { 1 },
                pid: my_pid,
            };
            // SAFETY: write exactly sizeof(StartupNotifyMsg) bytes to pipe fd.
            unsafe {
                let bytes = std::slice::from_raw_parts(
                    &msg as *const _ as *const u8,
                    std::mem::size_of::<StartupNotifyMsg>(),
                );
                let mut written = 0;
                while written < bytes.len() {
                    let n = libc::write(
                        fd,
                        bytes.as_ptr().add(written) as *const libc::c_void,
                        bytes.len() - written,
                    );
                    if n > 0 {
                        written += n as usize;
                        continue;
                    }
                    if n < 0 && io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
                        continue;
                    }
                    break;
                }
                libc::close(fd);
            }
        }
    }

    /// Start supervisor, wait for readiness, refresh netlink state, and enter RUNNING state.
    pub fn start(&mut self) -> Result<(), io::Error> {
        if let Err(e) = self.supervisor.start(&mut self.reactor) {
            crate::log_error!("Failed to start service: {}", e);
            self.notify_startup(1);
            let _ = self.context.runtime_state_transition(RuntimeState::Failed);
            self.teardown();
            return Err(io::Error::other(e.to_string()));
        }

        let mut probes = match ApiProbes::new(&self.api) {
            Ok(p) => p,
            Err(e) => {
                crate::log_error!("Failed to initialize API probes: {}", e);
                self.notify_startup(1);
                let _ = self.context.runtime_state_transition(RuntimeState::Failed);
                self.teardown();
                return Err(io::Error::other(e.to_string()));
            }
        };

        if let Err(e) = self.supervisor.wait_ready(&mut self.reactor, &mut probes) {
            crate::log_error!("sing-box failed to reach READY");
            self.notify_startup(1);
            let _ = self.context.runtime_state_transition(RuntimeState::Failed);
            self.teardown();
            return Err(io::Error::other(e.to_string()));
        }

        // Reconcile an already-present VPN after sing-box has reached READY.
        self.netlink.refresh_state(&mut self.reactor);

        if self
            .context
            .runtime_state_transition(RuntimeState::Running)
            .is_err()
        {
            crate::log_error!("Failed to transition runtime state to RUNNING");
            self.notify_startup(1);
            self.teardown();
            return Err(io::Error::other("Failed to publish RUNNING state"));
        }

        crate::log_info!("ATPD control plane active; sing-box owns the ebpf-in datapath");
        crate::log_info!("ATPD control-plane Reactor running, entering event loop");
        self.notify_startup(0);
        Ok(())
    }

    /// Main Reactor event loop matching `run_event_loop` in `src/main.c`.
    pub fn run(&mut self) -> i32 {
        let mut events = Vec::new();

        while !self.shutdown_requested {
            events.clear();
            if let Err(e) = self.reactor.wait(&mut events) {
                if e.raw_os_error() == Some(libc::EINTR) {
                    continue;
                }
                crate::log_error!("Reactor wait failed: {}", e);
                break;
            }

            for ev in events.drain(..) {
                match ev {
                    Event::Io { token, events: _ } => {
                        if token == TOKEN_ROUTE || token == TOKEN_XFRM {
                            let changes = self.netlink.handle_io(
                                &mut self.reactor,
                                &mut self.context,
                                &self.config,
                                token,
                            );
                            for change in changes {
                                self.api.on_vpn_change(change);
                            }
                        } else if token == self.ipc.listen_token() || token >= TOKEN_IPC_CLIENT_BASE
                        {
                            let requests = self.ipc.handle_io(&mut self.reactor, token, &|| {
                                crate::status::collect(
                                    &self.config,
                                    &self.supervisor.snapshot(),
                                    &self.api.snapshot(),
                                    &self.context.vpn_snapshot(),
                                )
                            });
                            for req in requests {
                                match req {
                                    IpcRequest::Shutdown => {
                                        self.shutdown_requested = true;
                                        let _ = self
                                            .context
                                            .runtime_state_transition(RuntimeState::Stopping);
                                    }
                                    IpcRequest::Reload => {
                                        self.process_config_reload();
                                    }
                                }
                            }
                        }
                    }
                    Event::Timer(id) => {
                        let mut handled = false;
                        if let Some(changes) = self.netlink.on_timer(
                            &mut self.reactor,
                            &mut self.context,
                            &self.config,
                            id,
                        ) {
                            for change in changes {
                                self.api.on_vpn_change(change);
                            }
                            handled = true;
                        }

                        if !handled {
                            if let Ok(mut probes) = ApiProbes::new(&self.api) {
                                if self.supervisor.on_timer(&mut self.reactor, id, &mut probes) {
                                    handled = true;
                                }
                            }
                        }

                        if !handled {
                            let _ = self.ipc.handle_timer(&mut self.reactor, id);
                        }
                    }
                    Event::Signal(sig) => match sig {
                        libc::SIGTERM | libc::SIGINT => {
                            self.shutdown_requested = true;
                            let _ = self
                                .context
                                .runtime_state_transition(RuntimeState::Stopping);
                        }
                        libc::SIGHUP => {
                            self.process_config_reload();
                        }
                        libc::SIGCHLD => {
                            self.supervisor.on_sigchld(&mut self.reactor);
                        }
                        libc::SIGUSR1 => {
                            let snap = crate::status::collect(
                                &self.config,
                                &self.supervisor.snapshot(),
                                &self.api.snapshot(),
                                &self.context.vpn_snapshot(),
                            );
                            let text =
                                crate::ui::dashboard::status_render_to_string(&snap, 0, false);
                            print!("{}", text);
                        }
                        _ => {}
                    },
                    Event::Wakeup => {
                        self.api.on_wakeup();
                    }
                }
            }
        }

        self.teardown();
        0
    }

    /// Transactional config reload matching `process_config_reload` in `src/main.c`.
    pub fn process_config_reload(&mut self) {
        if self.shutdown_requested {
            return;
        }
        if !self.context.can_reload() {
            return;
        }
        if self
            .context
            .runtime_state_transition(RuntimeState::Reloading)
            .is_err()
        {
            return;
        }

        crate::log_info!("Processing config reload...");
        let candidate = match crate::config::prepare_reload(&self.config_path) {
            Ok(c) => c,
            Err(_) => {
                crate::log_error!(
                    "Config reload result: INVALID_CANDIDATE; previous configuration retained"
                );
                self.reload_return_to_running_or_shutdown();
                return;
            }
        };

        let classification = crate::config::classify_reload(&self.config, &candidate);
        if classification
            .changes
            .contains(ReloadChanges::REQUIRES_RESTART)
        {
            let fields = if classification.changed_fields.is_empty() {
                "unclassified field"
            } else {
                &classification.changed_fields
            };
            crate::log_warn!(
                "Config reload result: REQUIRES_RESTART ({}); previous configuration retained",
                fields
            );
            self.reload_return_to_running_or_shutdown();
            return;
        }

        self.supervisor.apply_config(&mut self.reactor, &candidate);
        let _ = self.netlink.apply_vpn_bypass_config(&candidate);
        crate::utils::time::atp_timezone_init();
        self.config = candidate;
        crate::log_info!("Config reload completed successfully");
        self.reload_return_to_running_or_shutdown();
    }

    fn reload_return_to_running_or_shutdown(&mut self) {
        if self
            .context
            .runtime_state_transition(RuntimeState::Running)
            .is_ok()
        {
            return;
        }
        crate::log_error!("Config reload could not restore RUNNING state; shutting down");
        self.shutdown_requested = true;
    }

    /// Teardown components in exact reverse order matching `src/main.c` lines 523-543.
    pub fn teardown(&mut self) {
        crate::log_info!("Reactor exited, cleaning up...");
        self.ipc.cleanup(&mut self.reactor);
        let _ = self.supervisor.stop(&mut self.reactor);
        self.api.shutdown();
        self.netlink.close(&mut self.reactor);

        let _ = self.context.runtime_state_transition(RuntimeState::Stopped);

        if self.pid_fd >= 0 {
            // SAFETY: closing pid file descriptor.
            unsafe { libc::close(self.pid_fd) };
            self.pid_fd = -1;
            if let Ok(c_path) = std::ffi::CString::new(self.pid_file.as_os_str().as_bytes()) {
                // SAFETY: unlinking pid file from filesystem.
                unsafe { libc::unlink(c_path.as_ptr()) };
            }
        }
        self.notify_startup(1);
    }
}

/// Double-fork daemonization protocol matching `daemonize` in `src/main.c`.
pub fn daemonize(config: &Config) -> io::Result<i32> {
    let mut startup_pipe = [0; 2];
    // SAFETY: pipe2 creates pipes with close-on-exec.
    let ret = unsafe { libc::pipe2(startup_pipe.as_mut_ptr(), libc::O_CLOEXEC) };
    if ret < 0 {
        eprintln!("Error: Failed to daemonize");
        return Ok(1);
    }

    // SAFETY: fork creates a child process.
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        // SAFETY: closing pipe fds on error.
        unsafe {
            libc::close(startup_pipe[0]);
            libc::close(startup_pipe[1]);
        }
        eprintln!("Error: Failed to daemonize");
        return Ok(1);
    }

    if pid > 0 {
        // Parent process: wait for notification from daemon child
        // SAFETY: closing unused write end of pipe.
        unsafe { libc::close(startup_pipe[1]) };

        let mut msg = StartupNotifyMsg { status: 1, pid: 0 };
        let mut read_bytes = 0;
        // SAFETY: creating slice to receive exactly sizeof(StartupNotifyMsg).
        let bytes = unsafe {
            std::slice::from_raw_parts_mut(
                &mut msg as *mut _ as *mut u8,
                std::mem::size_of::<StartupNotifyMsg>(),
            )
        };

        while read_bytes < bytes.len() {
            // SAFETY: reading notification message from pipe.
            let n = unsafe {
                libc::read(
                    startup_pipe[0],
                    bytes.as_mut_ptr().add(read_bytes) as *mut libc::c_void,
                    bytes.len() - read_bytes,
                )
            };
            if n > 0 {
                read_bytes += n as usize;
                continue;
            }
            if n < 0 && io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }
        // SAFETY: closing read end of pipe.
        unsafe { libc::close(startup_pipe[0]) };

        if read_bytes == std::mem::size_of::<StartupNotifyMsg>() && msg.status == 0 && msg.pid > 0 {
            // SAFETY: kill 0 verifies whether target pid exists.
            let is_alive = unsafe { libc::kill(msg.pid, 0) == 0 };
            if is_alive && process_is_atpd(msg.pid) {
                println!("Daemon started successfully (PID: {})", msg.pid);
                let socket_path = resolve_socket_path(config);
                if let Ok(resp) = crate::ipc::query_daemon(&socket_path, "status-summary\n") {
                    print!("{}", resp);
                }
                return Ok(0);
            }
            eprintln!(
                "Error: Daemon process {} is not running after startup",
                msg.pid
            );
            return Ok(1);
        }

        eprintln!("Error: Daemon failed to start");
        return Ok(1);
    }

    // Intermediate child process
    // SAFETY: closing unused read end of pipe.
    unsafe { libc::close(startup_pipe[0]) };
    let notify_fd = startup_pipe[1];

    // SAFETY: setsid establishes child as session leader.
    if unsafe { libc::setsid() } < 0 {
        notify_startup_fd(notify_fd, 1);
        // SAFETY: _exit terminates immediate process without running atexit handlers.
        unsafe { libc::_exit(1) };
    }

    // SAFETY: second fork detaches from controlling terminal.
    let pid2 = unsafe { libc::fork() };
    if pid2 < 0 {
        notify_startup_fd(notify_fd, 1);
        // SAFETY: _exit terminates on fork failure.
        unsafe { libc::_exit(1) };
    }
    if pid2 > 0 {
        // Intermediate process exits cleanly
        // SAFETY: _exit terminates parent of second fork.
        unsafe { libc::_exit(0) };
    }

    // Grandchild daemon process
    // SAFETY: set umask 027 matching C daemonize.
    unsafe { libc::umask(0o027) };

    let dev_null = b"/dev/null\0";
    // SAFETY: open /dev/null for standard descriptor redirection.
    let null_fd = unsafe { libc::open(dev_null.as_ptr() as *const libc::c_char, libc::O_RDWR) };
    if null_fd >= 0 {
        // SAFETY: redirect stdin, stdout, stderr to /dev/null.
        unsafe {
            libc::dup2(null_fd, libc::STDIN_FILENO);
            libc::dup2(null_fd, libc::STDOUT_FILENO);
            libc::dup2(null_fd, libc::STDERR_FILENO);
            if null_fd > 2 {
                libc::close(null_fd);
            }
        }
    }

    // Return the notification descriptor so the daemon child can notify the parent upon readiness
    Ok(notify_fd)
}

fn notify_startup_fd(fd: RawFd, result: i32) {
    // SAFETY: getpid retrieves current pid.
    let my_pid = unsafe { libc::getpid() };
    let msg = StartupNotifyMsg {
        status: if result == 0 { 0 } else { 1 },
        pid: my_pid,
    };
    // SAFETY: write notification directly to raw file descriptor.
    unsafe {
        let bytes = std::slice::from_raw_parts(
            &msg as *const _ as *const u8,
            std::mem::size_of::<StartupNotifyMsg>(),
        );
        let _ = libc::write(fd, bytes.as_ptr() as *const libc::c_void, bytes.len());
        libc::close(fd);
    }
}

/// Read PID and starttime identity from PID file.
/// Rejects legacy PID-only files or malformed content.
pub fn read_pid_identity(pid_file: &Path) -> io::Result<PidIdentity> {
    let path_cstr = std::ffi::CString::new(pid_file.as_os_str().as_bytes())
        .map_err(|_| io::Error::from_raw_os_error(libc::EINVAL))?;

    // SAFETY: open with safe flags O_RDONLY | O_NOFOLLOW | O_CLOEXEC.
    let fd = unsafe {
        libc::open(
            path_cstr.as_ptr(),
            libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
        )
    };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }

    let mut buf = [0u8; 128];
    let mut used = 0;
    while used < buf.len() - 1 {
        // SAFETY: reading up to remaining buffer space.
        let n = unsafe {
            libc::read(
                fd,
                buf.as_mut_ptr().add(used) as *mut libc::c_void,
                buf.len() - 1 - used,
            )
        };
        if n > 0 {
            used += n as usize;
            continue;
        }
        if n == 0 {
            break;
        }
        let err = io::Error::last_os_error();
        if err.raw_os_error() == Some(libc::EINTR) {
            continue;
        }
        // SAFETY: close descriptor on read error.
        unsafe { libc::close(fd) };
        return Err(err);
    }
    // SAFETY: close descriptor after read.
    unsafe { libc::close(fd) };

    if used == 0 {
        return Err(io::Error::from_raw_os_error(libc::EINVAL));
    }

    let s = std::str::from_utf8(&buf[..used])
        .map_err(|_| io::Error::from_raw_os_error(libc::EINVAL))?;
    let tokens: Vec<&str> = s.split_whitespace().collect();
    if tokens.len() != 2 {
        return Err(io::Error::from_raw_os_error(libc::EINVAL));
    }

    let pid = tokens[0]
        .parse::<i32>()
        .map_err(|_| io::Error::from_raw_os_error(libc::EINVAL))?;
    let starttime = tokens[1]
        .parse::<u64>()
        .map_err(|_| io::Error::from_raw_os_error(libc::EINVAL))?;

    if pid <= 0 || starttime == 0 {
        return Err(io::Error::from_raw_os_error(libc::EINVAL));
    }

    Ok(PidIdentity { pid, starttime })
}

/// Verify that `/proc/<pid>/exe` points to `atpd` or `atpd (deleted)`.
/// Strictly rejects prefix matches like `atpd-helper`.
pub fn process_is_atpd(pid: i32) -> bool {
    let path = format!("/proc/{}/exe", pid);
    let path_cstr = match std::ffi::CString::new(path) {
        Ok(c) => c,
        Err(_) => return false,
    };
    let mut buf = [0u8; libc::PATH_MAX as usize];
    let mut len: isize = -1;

    for _ in 0..3 {
        // SAFETY: readlink into stack buffer.
        len = unsafe {
            libc::readlink(
                path_cstr.as_ptr(),
                buf.as_mut_ptr() as *mut libc::c_char,
                buf.len() - 1,
            )
        };
        if len >= 0 {
            break;
        }
        if io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
            return false;
        }
    }

    if len <= 0 {
        return false;
    }

    let exe_str = match std::str::from_utf8(&buf[..len as usize]) {
        Ok(s) => s,
        Err(_) => return false,
    };
    let base = match Path::new(exe_str).file_name().and_then(|n| n.to_str()) {
        Some(b) => b,
        None => return false,
    };

    base == "atpd" || base == "atpd (deleted)"
}

/// Match running process against stored identity with TOCTOU starttime sandwich check.
pub fn process_matches_identity(identity: &PidIdentity) -> bool {
    if identity.pid <= 0 || identity.starttime == 0 {
        return false;
    }

    let starttime_before = match crate::utils::proc::process_starttime(identity.pid) {
        Ok(st) => st,
        Err(_) => return false,
    };
    if starttime_before != identity.starttime {
        return false;
    }

    if !process_is_atpd(identity.pid) {
        return false;
    }

    let starttime_after = match crate::utils::proc::process_starttime(identity.pid) {
        Ok(st) => st,
        Err(_) => return false,
    };

    starttime_after == identity.starttime
}

/// Atomically write process identity and take advisory write lock on PID file.
pub fn write_pid_file(pid_file: &Path) -> io::Result<RawFd> {
    // SAFETY: getpid retrieves current pid.
    let pid = unsafe { libc::getpid() };
    let starttime = crate::utils::proc::process_starttime(pid)?;
    if starttime == 0 {
        eprintln!("Error: Failed to read daemon process identity");
        return Err(io::Error::other("Failed to read daemon process identity"));
    }

    if let Some(parent) = pid_file.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }

    let path_cstr = std::ffi::CString::new(pid_file.as_os_str().as_bytes())
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;

    // SAFETY: open with flags O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, mode 0644.
    let fd = unsafe {
        libc::open(
            path_cstr.as_ptr(),
            libc::O_RDWR | libc::O_CREAT | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            0o644,
        )
    };
    if fd < 0 {
        let err = io::Error::last_os_error();
        eprintln!("Error: Failed to open PID file: {}", err);
        return Err(err);
    }

    // SAFETY: zeroed flock struct for fcntl write lock.
    let mut fl: libc::flock = unsafe { std::mem::zeroed() };
    fl.l_type = libc::F_WRLCK as libc::c_short;
    fl.l_whence = libc::SEEK_SET as libc::c_short;
    fl.l_start = 0;
    fl.l_len = 0;

    // SAFETY: fcntl F_SETLK requests non-blocking write lock.
    let lock_ret = unsafe { libc::fcntl(fd, libc::F_SETLK, &fl) };
    if lock_ret < 0 {
        let err = io::Error::last_os_error();
        // SAFETY: closing fd after lock failure.
        unsafe { libc::close(fd) };
        if err.raw_os_error() == Some(libc::EACCES) || err.raw_os_error() == Some(libc::EAGAIN) {
            eprintln!("Daemon is already running (PID file locked by another instance)");
        } else {
            eprintln!("Error: Failed to lock PID file: {}", err);
        }
        return Err(err);
    }

    let content = format!("{}\n{}\n", pid, starttime);
    let bytes = content.as_bytes();

    let written =
        // SAFETY: pwrite content at offset 0.
        unsafe { libc::pwrite(fd, bytes.as_ptr() as *const libc::c_void, bytes.len(), 0) };
    if written != bytes.len() as isize {
        let err = io::Error::last_os_error();
        // SAFETY: closing fd and unlinking file on write failure.
        unsafe {
            libc::close(fd);
            libc::unlink(path_cstr.as_ptr());
        }
        eprintln!("Error: Failed to write process identity to PID file");
        return Err(err);
    }

    // SAFETY: ftruncate truncates file to exact content length.
    let trunc_ret = unsafe { libc::ftruncate(fd, bytes.len() as libc::off_t) };
    if trunc_ret < 0 {
        let err = io::Error::last_os_error();
        // SAFETY: closing fd and unlinking file on ftruncate failure.
        unsafe {
            libc::close(fd);
            libc::unlink(path_cstr.as_ptr());
        }
        eprintln!("Error: Failed to write process identity to PID file");
        return Err(err);
    }

    // SAFETY: fsync flushes PID content to disk.
    let sync_ret = unsafe { libc::fsync(fd) };
    if sync_ret < 0 {
        let err = io::Error::last_os_error();
        // SAFETY: closing fd and unlinking file on fsync failure.
        unsafe {
            libc::close(fd);
            libc::unlink(path_cstr.as_ptr());
        }
        eprintln!("Error: Failed to write process identity to PID file");
        return Err(err);
    }

    Ok(fd)
}

pub fn resolve_pid_path(opts: &CliOptions, config: &Config) -> PathBuf {
    if !opts.pid_file.is_empty() {
        PathBuf::from(&opts.pid_file)
    } else {
        let configured = if !config.core.pid_file.is_empty() {
            &config.core.pid_file
        } else {
            crate::config::ATP_PID_FILE
        };
        if configured.starts_with('/') {
            PathBuf::from(configured)
        } else {
            let data_dir = if !config.core.data_dir.is_empty() {
                &config.core.data_dir
            } else {
                "."
            };
            PathBuf::from(data_dir).join(configured)
        }
    }
}

pub fn resolve_socket_path(config: &Config) -> PathBuf {
    let run_dir = if !config.core.run_dir.is_empty() {
        &config.core.run_dir
    } else {
        crate::config::ATP_RUN_DIR
    };
    if run_dir.starts_with('/') {
        PathBuf::from(run_dir).join("atpd.sock")
    } else {
        let data_dir = if !config.core.data_dir.is_empty() {
            &config.core.data_dir
        } else {
            "."
        };
        PathBuf::from(data_dir).join(run_dir).join("atpd.sock")
    }
}

pub fn resolve_config_path(opts: &CliOptions, config: &Config) -> PathBuf {
    let path = if !opts.config_file.is_empty() {
        PathBuf::from(&opts.config_file)
    } else {
        let data_dir = if !config.core.data_dir.is_empty() {
            &config.core.data_dir
        } else {
            "."
        };
        PathBuf::from(data_dir).join("atp.conf")
    };
    if path.is_absolute() {
        path
    } else if let Ok(cwd) = std::env::current_dir() {
        cwd.join(path)
    } else {
        path
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_read_pid_identity_formats() {
        let dir = tempfile_dir("test_pid_identity");
        let pid_file = dir.join("atpd.pid");

        // 1. Valid format
        fs::write(&pid_file, "12345\n67890\n").expect("write pid");
        let id = read_pid_identity(&pid_file).expect("read identity");
        assert_eq!(id.pid, 12345);
        assert_eq!(id.starttime, 67890);

        // 2. Legacy single line (must fail safe)
        fs::write(&pid_file, "12345\n").expect("write legacy");
        assert!(read_pid_identity(&pid_file).is_err());

        // 3. Extra tokens (must fail)
        fs::write(&pid_file, "12345 67890 extra\n").expect("write extra");
        assert!(read_pid_identity(&pid_file).is_err());

        // 4. Invalid numbers (must fail)
        fs::write(&pid_file, "abc def\n").expect("write invalid");
        assert!(read_pid_identity(&pid_file).is_err());

        // 5. Zero starttime (must fail)
        fs::write(&pid_file, "12345 0\n").expect("write zero");
        assert!(read_pid_identity(&pid_file).is_err());

        // 6. Negative PID (must fail)
        fs::write(&pid_file, "-5 67890\n").expect("write negative");
        assert!(read_pid_identity(&pid_file).is_err());

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn test_process_matches_identity_logic() {
        // SAFETY: getpid is safe.
        let self_pid = unsafe { libc::getpid() };
        let self_starttime = crate::utils::proc::process_starttime(self_pid).expect("starttime");

        let id = PidIdentity {
            pid: self_pid,
            starttime: self_starttime,
        };
        // In test binary, exe name is not atpd, so process_is_atpd is false
        assert!(!process_matches_identity(&id));

        // Starttime mismatch
        let id_mismatch = PidIdentity {
            pid: self_pid,
            starttime: self_starttime + 1,
        };
        assert!(!process_matches_identity(&id_mismatch));

        // Nonexistent PID
        let id_nonexistent = PidIdentity {
            pid: 999999,
            starttime: 100,
        };
        assert!(!process_matches_identity(&id_nonexistent));
    }

    #[test]
    fn test_reload_decision() {
        let base_cfg = Config::default();
        let mut candidate = base_cfg.clone();

        // Hot change
        candidate.core.ui_emoji_enabled = !base_cfg.core.ui_emoji_enabled;
        let classification = crate::config::classify_reload(&base_cfg, &candidate);
        assert!(!classification
            .changes
            .contains(ReloadChanges::REQUIRES_RESTART));

        // Restart-required change
        candidate.api.port = base_cfg.api.port + 1;
        let classification = crate::config::classify_reload(&base_cfg, &candidate);
        assert!(classification
            .changes
            .contains(ReloadChanges::REQUIRES_RESTART));
        assert!(classification.changed_fields.contains("API_PORT"));
    }

    fn tempfile_dir(prefix: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("{}_{}", prefix, std::process::id()));
        let _ = fs::create_dir_all(&dir);
        dir
    }
}
