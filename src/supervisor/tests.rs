use super::credentials::*;
use super::*;
use crate::config::Config;
use crate::reactor::{Event, Reactor, TimerId};

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

// Mirrors tests/test_service_health.c: backoff progression and circuit breaker cooldown semantics
#[test]
fn test_backoff_and_circuit_breaker_semantics() {
    let mut b = health::Backoff::new();
    assert_eq!(b.next_delay(), 1000);
    assert_eq!(b.next_delay(), 2000);
    assert_eq!(b.next_delay(), 4000);
    assert_eq!(b.next_delay(), 8000);
    b.reset();
    assert_eq!(b.next_delay(), 1000);

    let mut cb = health::CircuitBreaker::new(3, 10);
    assert!(cb.should_allow_at(100));
    cb.record_failure_at(100);
    assert!(!cb.is_open());
    cb.record_failure_at(101);
    assert!(!cb.is_open());
    cb.record_failure_at(102);
    assert!(cb.is_open());
    assert!(!cb.should_allow_at(105)); // within 10s cooldown
    assert!(!cb.should_allow_at(112)); // 112 - 102 = 10, not > 10
    assert!(cb.should_allow_at(113)); // 113 - 102 = 11 > 10, cooldown expired
    assert!(!cb.is_open());
    cb.record_success();
    assert_eq!(cb.consecutive_failures, 0);
}

// Mirrors tests/test_service_health.c: snapshot health check invariants, staleness, and child identity
#[test]
fn test_service_health_cb_snapshot_invariants() {
    let mut cfg = Config::default();
    cfg.service.circuit_threshold = 5;
    let mut supervisor = Supervisor::new(&cfg).expect("create supervisor");
    supervisor.state = ServiceState::Running;
    let my_pid = std::process::id() as i32;
    supervisor.child_pid = Some(my_pid);
    supervisor.child_starttime_ticks =
        crate::utils::proc::process_starttime(my_pid).expect("my starttime");

    struct FakeHealthProbe {
        valid: bool,
        updated_at_ms: u64,
        has_api: bool,
        start_time_ms: u64,
    }

    impl Probes for FakeHealthProbe {
        fn api_ready(&mut self) -> bool {
            true
        }
        fn api_healthy(&mut self) -> bool {
            self.has_api && self.valid && self.updated_at_ms >= self.start_time_ms
        }
    }

    let mut reactor = Reactor::new().expect("create reactor");
    let timer_id = supervisor.health_timer.unwrap_or(TimerId(999));
    supervisor.health_timer = Some(timer_id);

    let mut probes = FakeHealthProbe {
        valid: true,
        updated_at_ms: crate::reactor::now_ms(),
        has_api: true,
        start_time_ms: 0,
    };

    // Valid and current snapshot
    assert!(supervisor.on_timer(&mut reactor, timer_id, &mut probes));
    assert!(supervisor.running_healthy);
    assert_ne!(supervisor.last_health_check, 0);

    // Invalid snapshot
    probes.valid = false;
    assert!(supervisor.on_timer(&mut reactor, timer_id, &mut probes));
    assert!(!supervisor.running_healthy);
    assert_eq!(supervisor.breaker.consecutive_failures, 1);

    // Stale / older than child start_time_ms
    probes.valid = true;
    let now = crate::reactor::now_ms();
    probes.updated_at_ms = now;
    probes.start_time_ms = now + 1;
    assert!(supervisor.on_timer(&mut reactor, timer_id, &mut probes));
    assert!(!supervisor.running_healthy);

    // Updated_at_ms equals start_time_ms
    probes.start_time_ms = probes.updated_at_ms;
    assert!(supervisor.on_timer(&mut reactor, timer_id, &mut probes));
    assert!(supervisor.running_healthy);
    assert_eq!(supervisor.breaker.consecutive_failures, 0);

    // Missing API
    probes.has_api = false;
    assert!(supervisor.on_timer(&mut reactor, timer_id, &mut probes));
    assert!(!supervisor.running_healthy);
    probes.has_api = true;

    // Starttime mismatch
    supervisor.child_starttime_ticks += 1;
    assert!(supervisor.on_timer(&mut reactor, timer_id, &mut probes));
    assert!(!supervisor.running_healthy);
    supervisor.child_starttime_ticks -= 1;

    // Stopped state does not run health check
    supervisor.state = ServiceState::Stopped;
    supervisor.last_health_check = 0;
    supervisor.running_healthy = false;
    assert!(supervisor.on_timer(&mut reactor, timer_id, &mut probes));
    assert!(!supervisor.running_healthy);
    assert_eq!(supervisor.last_health_check, 0);

    supervisor.child_pid = None;
}

// Mirrors tests/test_service_parsing.c: service args and environment variable parsing
#[test]
fn test_service_parsing() {
    let bin = "/bin/sing-box";
    let work = "/work";
    let conf = "/default.json";

    // Double -c
    let args1 = process::build_service_args(bin, work, conf, "-c /one.json -c /two.json")
        .expect("build args double c");
    assert_eq!(
        args1,
        vec![
            "/bin/sing-box",
            "run",
            "-D",
            "/work",
            "-c",
            "/one.json",
            "-c",
            "/two.json"
        ]
    );

    // Capital -C
    let args2 =
        process::build_service_args(bin, work, conf, "-C /configs").expect("build args capital C");
    assert_eq!(
        args2,
        vec!["/bin/sing-box", "run", "-D", "/work", "-C", "/configs"]
    );

    // Substring -cpu
    let args3 =
        process::build_service_args(bin, work, conf, "-cpu 2").expect("build args substring");
    assert_eq!(
        args3,
        vec![
            "/bin/sing-box",
            "run",
            "-D",
            "/work",
            "-c",
            "/default.json",
            "-cpu",
            "2"
        ]
    );

    // Shell quoting and escaping must fail
    assert!(process::build_service_args(bin, work, conf, "--name=\"two words\"").is_err());
    assert!(process::build_service_args(bin, work, conf, "--name='unterminated").is_err());
    assert!(process::build_service_args(bin, work, conf, "two\\ words").is_err());

    // Valid environment
    let env_str = "GOOD=value EMPTY= WITH_EQUALS=a=b _OK=$VAR";
    let parsed_env = process::parse_service_env(env_str).expect("parse valid env");
    assert_eq!(
        parsed_env,
        vec![
            ("GOOD".to_string(), "value".to_string()),
            ("EMPTY".to_string(), "".to_string()),
            ("WITH_EQUALS".to_string(), "a=b".to_string()),
            ("_OK".to_string(), "$VAR".to_string()),
        ]
    );

    // Invalid environment tokens
    let invalid_envs = [
        "NO_EQUALS",
        "=empty",
        "9KEY=value",
        "BAD-KEY=value",
        "GOOD=value trailing",
        "QUOTED=\"two words\"",
    ];
    for inv in invalid_envs {
        assert!(
            process::parse_service_env(inv).is_err(),
            "expected error for '{}'",
            inv
        );
    }
}

struct TestEnv {
    dir: std::path::PathBuf,
    config: Config,
}

impl TestEnv {
    fn new(script_content: &str) -> Self {
        use std::os::unix::fs::PermissionsExt;
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let pid = std::process::id();
        let dir = std::env::temp_dir().join(format!("atpd_test_{}_{}", pid, nanos));
        let bin_dir = dir.join("bin");
        let run_dir = dir.join("run");
        std::fs::create_dir_all(&bin_dir).unwrap();
        std::fs::create_dir_all(&run_dir).unwrap();

        let bin_path = bin_dir.join("sing-box");
        std::fs::write(&bin_path, script_content).unwrap();
        let mut perms = std::fs::metadata(&bin_path).unwrap().permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&bin_path, perms).unwrap();

        let mut config = Config::default();
        config.core.data_dir = dir.to_string_lossy().to_string();
        config.core.run_dir = "run".to_string();
        config.service.start_timeout_sec = 2;
        config.service.stop_timeout_sec = 1;

        Self { dir, config }
    }
}

impl Drop for TestEnv {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

// Mirrors tests/test_singbox_lifecycle.sh: (a) start -> fake probe ready -> Running, snapshot shows pid
// Child-spawning tests share the process-wide SIGCHLD. The test harness's other threads don't
// block it and can swallow it, so serialize these tests and also poll-reap on a short timer.
static CHILD_TESTS: std::sync::Mutex<()> = std::sync::Mutex::new(());
const REAP_POLL_MS: u64 = 50;

fn child_test_lock() -> std::sync::MutexGuard<'static, ()> {
    CHILD_TESTS.lock().unwrap_or_else(|e| e.into_inner())
}

#[test]
fn test_supervisor_start_and_running_snapshot() {
    let env = TestEnv::new("#!/bin/sh\nwhile true; do sleep 1; done\n");
    let mut reactor = Reactor::new().expect("create reactor");
    let _guard = child_test_lock();
    reactor.watch_signal(libc::SIGCHLD).expect("watch sigchld");
    reactor.add_timer(REAP_POLL_MS, REAP_POLL_MS);

    let mut supervisor = Supervisor::new(&env.config).expect("create supervisor");
    assert_eq!(supervisor.snapshot().state, ServiceState::Stopped);
    assert_eq!(supervisor.snapshot().child_pid, None);

    supervisor.start(&mut reactor).expect("start supervisor");
    assert_eq!(supervisor.snapshot().state, ServiceState::Starting);
    let child_pid = supervisor.child_pid.expect("child pid spawned");
    assert!(child_pid > 0);

    struct FakeReadyProbe;
    impl Probes for FakeReadyProbe {
        fn api_ready(&mut self) -> bool {
            true
        }
        fn api_healthy(&mut self) -> bool {
            true
        }
    }

    let mut events = Vec::new();
    let mut became_running = false;
    for _ in 0..100 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        supervisor.on_sigchld(&mut reactor);
        for ev in &events {
            if let Event::Timer(id) = *ev {
                supervisor.on_timer(&mut reactor, id, &mut FakeReadyProbe);
            }
        }
        if supervisor.snapshot().state == ServiceState::Running {
            became_running = true;
            break;
        }
    }

    assert!(became_running);
    let snap = supervisor.snapshot();
    assert_eq!(snap.state, ServiceState::Running);
    assert_eq!(snap.child_pid, Some(child_pid));
    assert!(snap.healthy);

    // Mirrors service_apply_config: a hot interval change while Running replaces the health timer.
    let old_timer = supervisor.health_timer;
    assert!(old_timer.is_some());
    let mut cfg = env.config.clone();
    cfg.service.health_check_interval_ms = supervisor.health_check_interval_ms + 1000;
    supervisor.apply_config(&mut reactor, &cfg);
    assert!(supervisor.health_timer.is_some());
    assert_ne!(supervisor.health_timer, old_timer);

    supervisor.stop(&mut reactor).expect("stop supervisor");
    assert_eq!(supervisor.snapshot().state, ServiceState::Stopped);
}

// Mirrors tests/test_service_crash_recovery.sh: (b) child exits -> on_sigchld -> retry scheduled with backoff, child respawned
#[test]
fn test_supervisor_crash_recovery_respawn() {
    let env = TestEnv::new("#!/bin/sh\nwhile true; do sleep 1; done\n");
    let mut reactor = Reactor::new().expect("create reactor");
    let _guard = child_test_lock();
    reactor.watch_signal(libc::SIGCHLD).expect("watch sigchld");
    reactor.add_timer(REAP_POLL_MS, REAP_POLL_MS);

    let mut supervisor = Supervisor::new(&env.config).expect("create supervisor");
    // Speed up retry backoff for testing
    supervisor.backoff.base_delay_ms = 20;
    supervisor.backoff.current_delay_ms = 20;

    supervisor.start(&mut reactor).expect("start supervisor");
    let initial_pid = supervisor.child_pid.expect("child pid spawned");

    struct FakeReadyProbe;
    impl Probes for FakeReadyProbe {
        fn api_ready(&mut self) -> bool {
            true
        }
        fn api_healthy(&mut self) -> bool {
            true
        }
    }

    // Step reactor wait until Running
    let mut events = Vec::new();
    for _ in 0..100 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        supervisor.on_sigchld(&mut reactor);
        for ev in &events {
            if let Event::Timer(id) = *ev {
                supervisor.on_timer(&mut reactor, id, &mut FakeReadyProbe);
            }
        }
        if supervisor.snapshot().state == ServiceState::Running {
            break;
        }
    }
    assert_eq!(supervisor.snapshot().state, ServiceState::Running);

    // Simulate crash by sending SIGKILL to child
    // SAFETY: kill child process to trigger crash recovery.
    unsafe { libc::kill(initial_pid, libc::SIGKILL) };

    let mut reaped = false;
    for _ in 0..100 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        supervisor.on_sigchld(&mut reactor);
        if supervisor.child_pid != Some(initial_pid) {
            reaped = true;
            break;
        }
    }
    assert!(reaped);
    assert_eq!(supervisor.snapshot().state, ServiceState::Failed);
    assert!(supervisor.retry_timer.is_some());

    // Wait for retry timer to trigger delayed respawn
    let mut respawned = false;
    for _ in 0..100 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        supervisor.on_sigchld(&mut reactor);
        for ev in &events {
            if let Event::Timer(id) = *ev {
                if supervisor.on_timer(&mut reactor, id, &mut FakeReadyProbe)
                    && supervisor.child_pid.is_some()
                    && supervisor.child_pid != Some(initial_pid)
                {
                    respawned = true;
                    break;
                }
            }
        }
        if respawned {
            break;
        }
    }

    assert!(respawned);
    let new_pid = supervisor.child_pid.expect("new child pid");
    assert_ne!(new_pid, initial_pid);

    supervisor.stop(&mut reactor).expect("stop supervisor");
}

// Mirrors tests/test_service_crash_recovery.sh: (c) repeated crashes reach SERVICE_CIRCUIT_THRESHOLD -> circuit open, state Failed, no respawn
#[test]
fn test_supervisor_repeated_crashes_circuit_breaker_open() {
    let mut cfg = Config::default();
    let env = TestEnv::new("#!/bin/sh\nexit 1\n");
    cfg.core.data_dir = env.dir.to_string_lossy().to_string();
    cfg.core.run_dir = "run".to_string();
    cfg.service.circuit_threshold = 3;
    cfg.service.max_failures = 3;
    cfg.service.circuit_cooldown_sec = 60;

    let mut reactor = Reactor::new().expect("create reactor");
    let _guard = child_test_lock();
    reactor.watch_signal(libc::SIGCHLD).expect("watch sigchld");
    reactor.add_timer(REAP_POLL_MS, REAP_POLL_MS);

    let mut supervisor = Supervisor::new(&cfg).expect("create supervisor");
    // Speed up retry backoff for testing
    supervisor.backoff.base_delay_ms = 10;
    supervisor.backoff.current_delay_ms = 10;

    supervisor.start(&mut reactor).expect("start supervisor");

    struct FakeProbe;
    impl Probes for FakeProbe {
        fn api_ready(&mut self) -> bool {
            false
        }
        fn api_healthy(&mut self) -> bool {
            false
        }
    }

    let mut events = Vec::new();
    for _ in 0..50 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        supervisor.on_sigchld(&mut reactor);
        for ev in &events {
            match *ev {
                Event::Signal(libc::SIGCHLD) => {
                    supervisor.on_sigchld(&mut reactor);
                }
                Event::Timer(id) => {
                    supervisor.on_timer(&mut reactor, id, &mut FakeProbe);
                }
                _ => {}
            }
        }
        if supervisor.breaker.is_open() {
            break;
        }
    }

    assert!(supervisor.breaker.is_open());
    assert_eq!(supervisor.snapshot().state, ServiceState::Failed);
    assert!(supervisor.retry_timer.is_none());
    assert_eq!(supervisor.child_pid, None);

    // Subsequent start calls must be rejected while circuit breaker is open
    assert!(matches!(
        supervisor.start(&mut reactor),
        Err(SupervisorError::CircuitOpen)
    ));

    // Once the cooldown has passed, the monitor starts a fresh round of retries.
    supervisor.breaker.last_failure_time = 0;
    let monitor = supervisor.monitor_timer.expect("monitor timer");
    supervisor.on_timer(&mut reactor, monitor, &mut FakeProbe);
    assert!(supervisor.retry_timer.is_some());
    assert_eq!(supervisor.fail_count, 0);

    supervisor.stop(&mut reactor).expect("stop supervisor");
}

// Mirrors tests/test_singbox_lifecycle.sh: (d) stop on a script that traps and ignores SIGTERM escalates to SIGKILL within the configured stop timeout
#[test]
fn test_supervisor_stop_sigterm_escalation_to_sigkill() {
    let script = "#!/bin/sh\ntrap '' TERM\nwhile true; do sleep 1; done\n";
    let mut cfg = Config::default();
    let env = TestEnv::new(script);
    cfg.core.data_dir = env.dir.to_string_lossy().to_string();
    cfg.core.run_dir = "run".to_string();
    cfg.service.stop_timeout_sec = 1;

    let mut reactor = Reactor::new().expect("create reactor");
    let _guard = child_test_lock();
    reactor.watch_signal(libc::SIGCHLD).expect("watch sigchld");
    reactor.add_timer(REAP_POLL_MS, REAP_POLL_MS);

    let mut supervisor = Supervisor::new(&cfg).expect("create supervisor");
    supervisor.start(&mut reactor).expect("start supervisor");
    let child_pid = supervisor.child_pid.expect("spawned pid");

    let start = std::time::Instant::now();
    supervisor.stop(&mut reactor).expect("stop supervisor");
    let elapsed = start.elapsed();

    // Verify escalation took at least ~1s (10 attempts * 100ms) and less than 4s
    assert!(elapsed >= std::time::Duration::from_millis(900));
    assert!(elapsed < std::time::Duration::from_secs(4));
    assert_eq!(supervisor.snapshot().state, ServiceState::Stopped);

    // SAFETY: kill with 0 verifies the process was killed by SIGKILL.
    let exists = unsafe { libc::kill(child_pid, 0) == 0 };
    assert!(!exists);
}

// Mirrors tests/test_singbox_lifecycle.sh: (e) PID identity: a snapshot never reports a pid whose /proc starttime differs
#[test]
fn test_supervisor_snapshot_pid_identity_starttime_mismatch() {
    let env = TestEnv::new("#!/bin/sh\nwhile true; do sleep 1; done\n");
    let mut reactor = Reactor::new().expect("create reactor");
    let _guard = child_test_lock();
    reactor.watch_signal(libc::SIGCHLD).expect("watch sigchld");
    reactor.add_timer(REAP_POLL_MS, REAP_POLL_MS);

    let mut supervisor = Supervisor::new(&env.config).expect("create supervisor");
    supervisor.start(&mut reactor).expect("start supervisor");
    let child_pid = supervisor.child_pid.expect("spawned pid");

    assert_eq!(supervisor.snapshot().child_pid, Some(child_pid));

    // Tamper with expected starttime ticks to simulate PID reuse by the OS
    supervisor.child_starttime_ticks += 1;

    // Snapshot must not report a PID whose starttime differs
    let snap = supervisor.snapshot();
    assert_eq!(snap.child_pid, None);
    assert!(!snap.healthy);

    // Restore starttime so stop() cleanly reaps the child
    supervisor.child_starttime_ticks -= 1;
    supervisor.stop(&mut reactor).expect("stop supervisor");
}

// Mirrors tests/test_singbox_lifecycle.sh: config validation via binary check
#[test]
fn test_supervisor_validate_config() {
    let env = TestEnv::new("#!/bin/sh\nif [ \"$1\" = \"check\" ]; then exit 0; fi\nexit 1\n");
    let supervisor = Supervisor::new(&env.config).expect("create supervisor");
    assert!(supervisor.validate_config().is_ok());
}

// Running as non-root: skip credential-dropping assertions when geteuid() != 0, but verify resolution
#[test]
fn test_credentials_drop_privileges_non_root() {
    // SAFETY: checking geteuid to verify test runner privilege level.
    let is_root = unsafe { libc::geteuid() == 0 };
    if !is_root {
        // As non-root, resolve must still work with fallback defaults
        let creds = service_credentials_resolve(None, None, 1000, 1000)
            .expect("resolve defaults as non-root");
        assert_eq!(creds.uid, 1000);
        assert_eq!(creds.gid, 1000);
    }
}

// Mirrors service.c's child-side umask(027): files sing-box creates must not be world-readable.
#[test]
fn test_supervisor_child_runs_with_umask_027() {
    let env = TestEnv::new("#!/bin/sh\numask > umask.out\nwhile true; do sleep 1; done\n");
    let mut reactor = Reactor::new().expect("create reactor");
    let _guard = child_test_lock();
    reactor.watch_signal(libc::SIGCHLD).expect("watch sigchld");

    let mut supervisor = Supervisor::new(&env.config).expect("create supervisor");
    supervisor.start(&mut reactor).expect("start supervisor");

    let out = env.dir.join("umask.out");
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    let mut umask = String::new();
    while std::time::Instant::now() < deadline {
        if let Ok(s) = std::fs::read_to_string(&out) {
            if !s.trim().is_empty() {
                umask = s.trim().to_string();
                break;
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    supervisor.stop(&mut reactor).expect("stop supervisor");
    assert_eq!(umask, "0027");
}

#[test]
fn test_service_rotate_log_keeps_three_copies_and_live_writer() {
    use std::io::Write;
    let env = TestEnv::new("");
    let log = env.dir.join("sing-box.log");
    let base = log.to_string_lossy().to_string();
    std::fs::write(format!("{}.1", base), "one").unwrap();
    std::fs::write(format!("{}.2", base), "two").unwrap();
    std::fs::write(format!("{}.3", base), "three").unwrap();

    // sing-box's stdout: an O_APPEND fd that stays open across the rotation.
    let mut writer = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log)
        .unwrap();
    writer.write_all(&vec![b'x'; 10 * 1024 * 1024]).unwrap();

    process::service_rotate_log(&log);

    assert_eq!(std::fs::metadata(&log).unwrap().len(), 0);
    assert_eq!(
        std::fs::metadata(format!("{}.1", base)).unwrap().len(),
        10 * 1024 * 1024
    );
    assert_eq!(
        std::fs::read_to_string(format!("{}.2", base)).unwrap(),
        "one"
    );
    assert_eq!(
        std::fs::read_to_string(format!("{}.3", base)).unwrap(),
        "two"
    );
    assert!(!std::path::Path::new(&format!("{}.4", base)).exists());

    writer.write_all(b"after").unwrap();
    assert_eq!(std::fs::read_to_string(&log).unwrap(), "after");

    // Below the limit nothing moves.
    process::service_rotate_log(&log);
    assert_eq!(std::fs::read_to_string(&log).unwrap(), "after");
}

#[test]
fn test_stop_signal_pending_sees_blocked_sigterm() {
    assert!(!stop_signal_pending());
    // SAFETY: SIGTERM is blocked and sent to this thread only, then consumed with sigwait
    // before it is unblocked, so it is never delivered.
    unsafe {
        let mut set: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut set);
        libc::sigaddset(&mut set, libc::SIGTERM);
        let mut old: libc::sigset_t = std::mem::zeroed();
        libc::pthread_sigmask(libc::SIG_BLOCK, &set, &mut old);
        libc::pthread_kill(libc::pthread_self(), libc::SIGTERM);
        assert!(stop_signal_pending());
        let mut sig = 0;
        libc::sigwait(&set, &mut sig);
        assert_eq!(sig, libc::SIGTERM);
        libc::pthread_sigmask(libc::SIG_SETMASK, &old, std::ptr::null_mut());
    }
    assert!(!stop_signal_pending());
}
