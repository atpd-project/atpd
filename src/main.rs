//! ATPD - Advanced Transparent Proxy Daemon Entrypoint.
//!
//! Mirrors CLI command routing and process lifecycle from `src/main.c`.

use std::io::{self, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::RawFd;
use std::path::{Path, PathBuf};

mod app;
#[allow(dead_code)]
mod cli;
#[allow(dead_code)]
mod config;
#[allow(dead_code)]
mod context;
#[allow(dead_code)]
mod ipc;
#[allow(dead_code)]
mod logger;
#[allow(dead_code)]
mod nativeapi;
#[allow(dead_code)]
mod netlink;
#[allow(dead_code)]
mod reactor;
#[allow(dead_code)]
mod status;
#[allow(dead_code)]
mod supervisor;
#[allow(dead_code)]
mod ui;
#[allow(dead_code)]
mod utils;
#[allow(dead_code)]
mod version;

use app::{
    daemonize, process_matches_identity, read_pid_identity, resolve_config_path, resolve_pid_path,
    resolve_socket_path, write_pid_file, App,
};
use cli::{CliOptions, Command, RunMode};
use config::Config;
use supervisor::Supervisor;

fn preflight_startup(config: &Config) -> i32 {
    let supervisor = match Supervisor::new(config) {
        Ok(s) => s,
        Err(_) => {
            eprintln!("Error: Failed to initialize service context");
            return 1;
        }
    };

    println!(
        "Checking sing-box configuration (binary: {}, config: {})...",
        supervisor.bin_path, supervisor.conf_path
    );

    if supervisor.validate_config().is_err() {
        println!("sing-box configuration check: FAIL");
        eprintln!("Error: sing-box configuration validation failed");
        return 1;
    }

    println!("sing-box configuration check: PASS");
    0
}

fn do_start(opts: &CliOptions, config: &Config, config_path: &Path) -> i32 {
    if preflight_startup(config) != 0 {
        return 1;
    }

    let pid_path = resolve_pid_path(opts, config);

    println!("Starting atpd...");
    let _ = io::stdout().flush();

    let mut startup_notify_fd: Option<RawFd> = None;
    if opts.run_mode != RunMode::Foreground {
        match daemonize(config) {
            Ok(0) => return 0,
            Ok(1) => return 1,
            Ok(fd) => {
                startup_notify_fd = Some(fd);
            }
            Err(_) => {
                eprintln!("Error: Failed to daemonize");
                return 1;
            }
        }
    }

    let pid_fd = match write_pid_file(&pid_path) {
        Ok(fd) => fd,
        Err(_) => {
            if let Some(fd) = startup_notify_fd {
                // SAFETY: close notification pipe on failure.
                unsafe { libc::close(fd) };
            }
            return 1;
        }
    };

    let mut app = match App::new(
        opts,
        config.clone(),
        config_path.to_path_buf(),
        pid_path,
        pid_fd,
        startup_notify_fd,
    ) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("Error: Failed to initialize application: {}", e);
            return 1;
        }
    };

    if app.start().is_err() {
        return 1;
    }

    app.run()
}

fn do_stop(opts: &CliOptions, config: &Config) -> i32 {
    let pid_path = resolve_pid_path(opts, config);

    let identity = match read_pid_identity(&pid_path) {
        Ok(id) => id,
        Err(e) => {
            if e.raw_os_error() == Some(libc::ENOENT) {
                if opts.command != Command::Restart {
                    eprintln!("Daemon is not running (no PID file)");
                }
                return 2;
            } else {
                eprintln!("Invalid or legacy PID file; refusing to signal");
            }
            return 1;
        }
    };

    let pid = identity.pid;
    if !process_matches_identity(&identity) {
        if opts.command == Command::Restart {
            return 2;
        }
        eprintln!(
            "Process {} identity does not match PID file (stale PID file)",
            pid
        );
        return 1;
    }

    // SAFETY: sending SIGTERM to daemon process.
    let kill_ret = unsafe { libc::kill(pid, libc::SIGTERM) };
    if kill_ret < 0 {
        let err = io::Error::last_os_error();
        if err.raw_os_error() == Some(libc::ESRCH) {
            if opts.command == Command::Restart {
                return 2;
            }
            eprintln!("Process {} not found (stale PID file)", pid);
            return 1;
        } else {
            eprintln!("kill: {}", err);
            return 1;
        }
    }

    let mut stopped = false;
    for _ in 0..50 {
        std::thread::sleep(std::time::Duration::from_millis(100));
        if !process_matches_identity(&identity) {
            stopped = true;
            break;
        }
    }

    if !stopped {
        if !process_matches_identity(&identity) {
            stopped = true;
        } else {
            eprintln!(
                "Process {} did not terminate gracefully, sending SIGKILL",
                pid
            );
            // SAFETY: sending SIGKILL to force termination.
            let kill_res = unsafe { libc::kill(pid, libc::SIGKILL) };
            if kill_res < 0 && io::Error::last_os_error().raw_os_error() != Some(libc::ESRCH) {
                eprintln!("kill(SIGKILL): {}", io::Error::last_os_error());
                return 1;
            }
        }
        for _ in 0..50 {
            if stopped {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(100));
            if !process_matches_identity(&identity) {
                stopped = true;
                break;
            }
        }
    }

    if !stopped {
        eprintln!("Error: Process {} could not be terminated", pid);
        return 1;
    }

    if let Ok(c_path) = std::ffi::CString::new(pid_path.as_os_str().as_bytes()) {
        // SAFETY: unlinking verified stopped pid file.
        unsafe { libc::unlink(c_path.as_ptr()) };
    }
    println!("Daemon stopped successfully");
    0
}

fn do_restart(opts: &CliOptions, config: &Config, config_path: &Path) -> i32 {
    println!("Restarting atpd...");
    let stop_rc = do_stop(opts, config);
    if stop_rc == 1 {
        return 1;
    }
    do_start(opts, config, config_path)
}

fn terminal_width() -> usize {
    // SAFETY: checking if stdout is a terminal and querying window size via ioctl.
    unsafe {
        if libc::isatty(libc::STDOUT_FILENO) == 1 {
            let mut ws: libc::winsize = std::mem::zeroed();
            if libc::ioctl(libc::STDOUT_FILENO, libc::TIOCGWINSZ, &mut ws) == 0 && ws.ws_col > 0 {
                let col = ws.ws_col as usize;
                return if col < 30 { 30 } else { col };
            }
        }
    }
    0
}

fn do_status(opts: &CliOptions, config: &Config) -> i32 {
    let socket_path = resolve_socket_path(config);
    let mut width = terminal_width();
    if width > 200 {
        width = 200;
    }
    let command = if width > 0 {
        format!("status {}\n", width)
    } else {
        "status\n".to_string()
    };

    if let Ok(resp) = crate::ipc::query_daemon(&socket_path, &command) {
        print!("{}", resp);
        return 0;
    }

    // Offline inspection when daemon is stopped
    let supervisor_snap = crate::supervisor::ServiceSnapshot {
        child_pid: None,
        state: crate::supervisor::ServiceState::Stopped,
        healthy: false,
        start_time: 0,
    };
    let api_snap = crate::nativeapi::ApiSnapshot::default();
    let vpn_snap = crate::context::VpnSnapshot {
        state: crate::context::VpnState::Idle,
        if_id: 0,
        iface: [0; 32],
        changed_at: libc::timespec {
            tv_sec: 0,
            tv_nsec: 0,
        },
        transitions: 0,
    };
    let snapshot = crate::status::collect(config, &supervisor_snap, &api_snap, &vpn_snap);
    let rendered = crate::ui::dashboard::status_render_to_string(&snapshot, width, opts.no_color);
    print!("{}", rendered);
    0
}

fn do_reload(opts: &CliOptions, config: &Config) -> i32 {
    let pid_path = resolve_pid_path(opts, config);
    let identity = match read_pid_identity(&pid_path) {
        Ok(id) => id,
        Err(e) => {
            if e.raw_os_error() == Some(libc::ENOENT) {
                eprintln!("Daemon is not running (no PID file)");
            } else {
                eprintln!("Invalid or legacy PID file; refusing to signal");
            }
            return 1;
        }
    };

    let pid = identity.pid;
    if !process_matches_identity(&identity) {
        eprintln!(
            "Process {} identity does not match PID file (stale PID file)",
            pid
        );
        return 1;
    }

    // SAFETY: sending SIGHUP to verified daemon process.
    let ret = unsafe { libc::kill(pid, libc::SIGHUP) };
    if ret < 0 {
        eprintln!("kill(SIGHUP): {}", io::Error::last_os_error());
        return 1;
    }

    println!("Reload signal sent to PID {}", pid);
    0
}

fn do_check(_opts: &CliOptions, config: &Config) -> i32 {
    println!("Validating ATPD configuration...");
    if crate::config::validator::config_validate_values(config).is_ok() {
        println!("Configuration is valid");
        0
    } else {
        println!("Configuration validation failed");
        1
    }
}

fn main() {
    crate::utils::time::atp_timezone_init();

    let args: Vec<String> = std::env::args().collect();
    let opts = match cli::parse_arguments(&args) {
        Ok(o) => o,
        Err(e) => {
            eprintln!("{}", e);
            std::process::exit(e.exit_code);
        }
    };

    if opts.command == Command::Version {
        let ver = cli::print_version();
        print!("{}", ver);
        std::process::exit(0);
    }
    if opts.command == Command::Help {
        let prog = args.first().map(|s| s.as_str()).unwrap_or("atpd");
        let usage = cli::print_usage(prog);
        print!("{}", usage);
        std::process::exit(0);
    }

    let mut config = Config::default();
    let (cfg_path, is_explicit) = if !opts.config_file.is_empty() {
        (PathBuf::from(&opts.config_file), true)
    } else {
        let data_dir = if !config.core.data_dir.is_empty() {
            &config.core.data_dir
        } else {
            "."
        };
        (PathBuf::from(data_dir).join("atp.conf"), false)
    };

    let resolved_config_path = resolve_config_path(&opts, &config);

    if cfg_path.exists() {
        match config::load(&cfg_path) {
            Ok(loaded) => {
                config = loaded;
            }
            Err(_) => {
                eprintln!("Invalid configuration: {}", cfg_path.display());
                std::process::exit(1);
            }
        }
    } else if is_explicit {
        eprintln!("Cannot read configuration: {}", cfg_path.display());
        std::process::exit(1);
    }

    let exit_code = match opts.command {
        Command::Start => do_start(&opts, &config, &resolved_config_path),
        Command::Stop => {
            if do_stop(&opts, &config) == 0 {
                0
            } else {
                1
            }
        }
        Command::Restart => do_restart(&opts, &config, &resolved_config_path),
        Command::Status => do_status(&opts, &config),
        Command::Reload => do_reload(&opts, &config),
        Command::Check => do_check(&opts, &config),
        _ => {
            let prog = args.first().map(|s| s.as_str()).unwrap_or("atpd");
            let usage = cli::print_usage(prog);
            print!("{}", usage);
            0
        }
    };

    std::process::exit(exit_code);
}
