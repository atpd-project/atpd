//! Integration and unit tests for sing-box Native API client, dispatcher, and supervisor probes.
//!
//! Mirrors `tests/test_api_host.c`, `tests/test_api_snapshot.c`, and `tests/test_api_vpn_mode.c`.

use super::client::{Client, ClientError};
use super::proto::{self, ProtoError};
use super::status::SingboxClashModeStatus;
use super::{is_clash_mode_supported, match_clash_mode, ApiHandle, ApiProbes};
use crate::config::{
    ApiConfig, Config, CoreConfig, InterfaceConfig, ServiceConfig, ATP_MAX_VPN_BYPASS_ROUTES,
};
use crate::context::{VpnChange, VpnState};
use crate::reactor::{Event, Reactor};
use crate::supervisor::Probes;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

fn default_test_config(port: u16) -> Config {
    Config {
        core: CoreConfig {
            ui_emoji_enabled: false,
            log_timestamp: false,
            data_dir: String::new(),
            run_dir: String::new(),
            core_user: String::new(),
            core_group: String::new(),
            pid_file: String::new(),
            log_file: String::new(),
        },
        interface: InterfaceConfig {
            vpn_auto_mode: true,
            vpn_target_mode: "Google VPN".to_string(),
            vpn_fallback_mode: "Rule".to_string(),
            vpn_bypass_routes: [crate::config::VpnBypassRoute {
                family: 0,
                addr: [0; 16],
                prefix_len: 0,
            }; ATP_MAX_VPN_BYPASS_ROUTES],
            vpn_bypass_route_count: 0,
            vpn_bypass_pref: 0,
        },
        service: ServiceConfig {
            restart_delay_sec: 1,
            start_timeout_sec: 5,
            stop_timeout_sec: 5,
            grace_period_sec: 1,
            max_failures: 5,
            circuit_threshold: 5,
            circuit_cooldown_sec: 60,
            health_check_interval_ms: 1000,
            args: String::new(),
            env: String::new(),
        },
        api: ApiConfig {
            port: port as i32,
            host: "127.0.0.1".to_string(),
            secret: String::new(),
        },
    }
}

#[derive(Debug, Clone)]
struct RecordedRequest {
    path: String,
    headers: HashMap<String, String>,
    body: Vec<u8>,
}

type RequestHandler = Arc<dyn Fn(&RecordedRequest) -> Option<Vec<u8>> + Send + Sync>;

struct FakeSingbox {
    port: u16,
    shutdown: Arc<AtomicBool>,
    recorded: Arc<Mutex<Vec<RecordedRequest>>>,
    server_thread: Option<thread::JoinHandle<()>>,
}

impl FakeSingbox {
    fn new(handler: RequestHandler) -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind listener");
        listener
            .set_nonblocking(true)
            .expect("set nonblocking listener");
        let port = listener.local_addr().expect("local addr").port();
        let shutdown = Arc::new(AtomicBool::new(false));
        let recorded = Arc::new(Mutex::new(Vec::new()));

        let shutdown_clone = Arc::clone(&shutdown);
        let recorded_clone = Arc::clone(&recorded);

        let server_thread = thread::spawn(move || {
            while !shutdown_clone.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((mut stream, _)) => {
                        let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
                        let _ = stream.set_write_timeout(Some(Duration::from_millis(500)));
                        handle_connection(&mut stream, &handler, &recorded_clone);
                    }
                    Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(10));
                    }
                    Err(_) => break,
                }
            }
        });

        Self {
            port,
            shutdown,
            recorded,
            server_thread: Some(server_thread),
        }
    }

    fn recorded_requests(&self) -> Vec<RecordedRequest> {
        let guard = match self.recorded.lock() {
            Ok(g) => g,
            Err(p) => p.into_inner(),
        };
        guard.clone()
    }
}

impl Drop for FakeSingbox {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::Relaxed);
        if let Some(t) = self.server_thread.take() {
            let _ = t.join();
        }
    }
}

fn handle_connection(
    stream: &mut TcpStream,
    handler: &RequestHandler,
    recorded: &Arc<Mutex<Vec<RecordedRequest>>>,
) {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 1024];
    let mut header_end = None;

    loop {
        match stream.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                    header_end = Some(pos);
                    break;
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(_) => return,
        }
    }

    let header_pos = match header_end {
        Some(pos) => pos,
        None => return,
    };

    let header_str = match std::str::from_utf8(&buf[..header_pos]) {
        Ok(s) => s,
        Err(_) => return,
    };

    let mut lines = header_str.lines();
    let first_line = match lines.next() {
        Some(l) => l,
        None => return,
    };

    let parts: Vec<&str> = first_line.split_whitespace().collect();
    if parts.len() < 2 {
        return;
    }
    let path = parts[1].to_string();

    let mut headers = HashMap::new();
    let mut content_length = 0;
    for line in lines {
        if let Some((k, v)) = line.split_once(':') {
            let key = k.trim().to_ascii_lowercase();
            let val = v.trim().to_string();
            if key == "content-length" {
                content_length = val.parse::<usize>().unwrap_or(0);
            }
            headers.insert(key, val);
        }
    }

    let mut body = buf[header_pos + 4..].to_vec();
    while body.len() < content_length {
        match stream.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => body.extend_from_slice(&chunk[..n]),
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(_) => break,
        }
    }

    let req = RecordedRequest {
        path,
        headers,
        body,
    };

    {
        let mut guard = match recorded.lock() {
            Ok(g) => g,
            Err(p) => p.into_inner(),
        };
        guard.push(req.clone());
    }

    if let Some(resp) = handler(&req) {
        let _ = stream.write_all(&resp);
    }
}

fn build_status_response(goroutines: i32, traffic_available: bool) -> Vec<u8> {
    let mut proto = Vec::new();
    // field 1: memory = 4096
    proto.push(0x08);
    proto::encode_varint(4096, &mut proto);
    // field 2: goroutines
    proto.push(0x10);
    proto::encode_varint(goroutines as u64, &mut proto);
    // field 5: traffic_available
    proto.push(0x28);
    proto::encode_varint(if traffic_available { 1 } else { 0 }, &mut proto);

    let mut frame = Vec::new();
    frame.push(0x00);
    frame.extend_from_slice(&(proto.len() as u32).to_be_bytes());
    frame.extend_from_slice(&proto);

    let header = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/grpc-web+proto\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        frame.len()
    );
    let mut resp = Vec::new();
    resp.extend_from_slice(header.as_bytes());
    resp.extend_from_slice(&frame);
    resp
}

fn build_version_response(version: &str) -> Vec<u8> {
    let mut proto = Vec::new();
    proto.push(0x0a);
    proto::encode_varint(version.len() as u64, &mut proto);
    proto.extend_from_slice(version.as_bytes());

    let mut frame = Vec::new();
    frame.push(0x00);
    frame.extend_from_slice(&(proto.len() as u32).to_be_bytes());
    frame.extend_from_slice(&proto);

    let header = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/grpc-web+proto\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        frame.len()
    );
    let mut resp = Vec::new();
    resp.extend_from_slice(header.as_bytes());
    resp.extend_from_slice(&frame);
    resp
}

fn build_clash_mode_status_response(modes: &[&str], current: &str) -> Vec<u8> {
    let mut proto = Vec::new();
    for m in modes {
        proto.push(0x0a);
        proto::encode_varint(m.len() as u64, &mut proto);
        proto.extend_from_slice(m.as_bytes());
    }
    proto.push(0x12);
    proto::encode_varint(current.len() as u64, &mut proto);
    proto.extend_from_slice(current.as_bytes());

    let mut frame = Vec::new();
    frame.push(0x00);
    frame.extend_from_slice(&(proto.len() as u32).to_be_bytes());
    frame.extend_from_slice(&proto);

    let header = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/grpc-web+proto\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        frame.len()
    );
    let mut resp = Vec::new();
    resp.extend_from_slice(header.as_bytes());
    resp.extend_from_slice(&frame);
    resp
}

fn build_empty_unary_response() -> Vec<u8> {
    let frame = [0x00, 0x00, 0x00, 0x00, 0x00];
    let header = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/grpc-web+proto\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        frame.len()
    );
    let mut resp = Vec::new();
    resp.extend_from_slice(header.as_bytes());
    resp.extend_from_slice(&frame);
    resp
}

// ============================================================================
// Acceptance Test: SetClashMode request-body byte test
// Covers exact request bytes: tag 0x1A, length varint, mode bytes
// ============================================================================
#[test]
fn test_set_clash_mode_request_body_exact_bytes() {
    let mode = "Google VPN";
    let encoded = proto::encode_set_clash_mode(mode).expect("encode clash mode");

    // Must be: Tag 0x1A, varint length (10 = 0x0A), mode bytes
    let mut expected = vec![0x1a, 0x0a];
    expected.extend_from_slice(mode.as_bytes());
    assert_eq!(
        encoded, expected,
        "SetClashMode body must exactly match field 3 tag 0x1A"
    );

    // Also verify via in-process fake sing-box over TCP stream
    let received_body = Arc::new(Mutex::new(Vec::new()));
    let received_body_clone = Arc::clone(&received_body);

    let fake = FakeSingbox::new(Arc::new(move |req| {
        if req.path == "/daemon.StartedService/SetClashMode" {
            let mut guard = received_body_clone.lock().expect("lock body");
            // Skip 5-byte gRPC-Web frame header to get the exact message body
            if req.body.len() >= 5 {
                *guard = req.body[5..].to_vec();
            }
            Some(build_empty_unary_response())
        } else {
            None
        }
    }));

    let client =
        Client::new("127.0.0.1", fake.port, "", Duration::from_secs(1)).expect("create client");
    client.set_clash_mode(mode).expect("set clash mode");

    let actual = received_body.lock().expect("lock body").clone();
    assert_eq!(actual, expected, "Wire request body must match exact bytes");
}

// ============================================================================
// Acceptance Test: Status frame parsing with truncated/oversized/garbage inputs rejected
// ============================================================================
#[test]
fn test_status_frame_parsing_corrupted_rejected() {
    // 1. Buffer too short (< 5 bytes) returns Ok(None)
    assert_eq!(proto::parse_status_frame(&[]), Ok(None));
    assert_eq!(proto::parse_status_frame(&[0x00, 0x00, 0x00]), Ok(None));

    // 2. Trailers frame (flags bit 0x80) returns Err(TrailersFrame)
    assert_eq!(
        proto::parse_status_frame(&[0x80, 0x00, 0x00, 0x00, 0x00]),
        Err(ProtoError::TrailersFrame)
    );

    // 3. Message length larger than buffer returns Ok(None) (waiting for more stream data)
    assert_eq!(
        proto::parse_status_frame(&[0x00, 0x00, 0x00, 0x10, 0x01, 0x02]),
        Ok(None)
    );

    // 4. Garbage varint exceeding 64-bit shift limit returns Err(InvalidVarint)
    let garbage_varint_frame = [
        0x00, 0x00, 0x00, 0x00, 0x0b, // 5-byte header, length 11
        0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
    ];
    assert!(
        proto::parse_status_frame(&garbage_varint_frame).is_err(),
        "Garbage overlong varint must be rejected"
    );

    // 5. Value out of range (> i32::MAX for goroutines field 2) returns Err(ValueOutOfRange)
    let overflow_goroutines = [
        0x00, 0x00, 0x00, 0x00, 0x06, // 5-byte header, length 6
        0x10, 0x80, 0x80, 0x80, 0x80, 0x08, // field 2 varint = 0x80000000 > i32::MAX
    ];
    assert_eq!(
        proto::parse_status_frame(&overflow_goroutines),
        Err(ProtoError::ValueOutOfRange)
    );

    // 6. Unknown wire type (e.g. wire type 7) returns Err(InvalidWireType)
    let invalid_wire_type = [
        0x00, 0x00, 0x00, 0x00, 0x02, // 5-byte header, length 2
        0x0f, 0x01, // tag = (1 << 3) | 7 (wire type 7)
    ];
    assert_eq!(
        proto::parse_status_frame(&invalid_wire_type),
        Err(ProtoError::InvalidWireType(7))
    );

    // 7. Length-delimited string length exceeding frame bounds returns Err(BufferOverflow)
    let overflow_string_field = [
        0x00, 0x00, 0x00, 0x00, 0x03, // 5-byte header, length 3
        0x12, 0x20, 0x41, // field 2 length-delimited with size 32, but only 1 byte present
    ];
    assert_eq!(
        proto::parse_string_field(&overflow_string_field, 2),
        Err(ProtoError::BufferOverflow)
    );
}

// ============================================================================
// Acceptance Test: Timeout when fake server accepts but never answers
// Must be bounded (~1 s) and not hang
// ============================================================================
#[test]
fn test_timeout_bounded_on_unresponsive_server() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind listener");
    let port = listener.local_addr().expect("local addr").port();
    let shutdown = Arc::new(AtomicBool::new(false));
    let shutdown_clone = Arc::clone(&shutdown);

    let server = thread::spawn(move || {
        let (stream, _) = listener.accept().expect("accept connection");
        // Hang connection without writing anything until test finishes
        while !shutdown_clone.load(Ordering::Relaxed) {
            thread::sleep(Duration::from_millis(50));
        }
        drop(stream);
    });

    let client =
        Client::new("127.0.0.1", port, "", Duration::from_millis(1000)).expect("create client");

    let start = Instant::now();
    let res = client.get_status();
    let elapsed = start.elapsed();

    shutdown.store(true, Ordering::Relaxed);
    let _ = server.join();

    assert!(res.is_err(), "Call against unresponsive server must fail");
    assert!(
        elapsed >= Duration::from_millis(900),
        "Elapsed time {:?} should be bounded near ~1s timeout",
        elapsed
    );
    assert!(
        elapsed < Duration::from_millis(2500),
        "Call must not hang indefinitely: elapsed was {:?}",
        elapsed
    );
}

// ============================================================================
// Ported Test: Mirrors tests/test_api_host.c
// Host validation, invalid host rejection without connecting, valid host routing.
// ============================================================================
#[test]
fn test_mirrors_test_api_host() {
    // 1. Invalid hosts rejected before socket creation (0 connections made)
    let invalid_hosts = ["192.0.2.999", "localhost", "::1"];
    for host in &invalid_hosts {
        let err = Client::new(host, 9080, "", Duration::from_secs(1));
        assert!(
            matches!(err, Err(ClientError::InvalidHost(_))),
            "Host '{}' must be rejected by IPv4 parser",
            host
        );
    }

    // 2. Valid IPv4 host connects to target port
    let fake = FakeSingbox::new(Arc::new(|req| {
        if req.path == "/daemon.StartedService/SubscribeStatus" {
            Some(build_status_response(10, true))
        } else {
            None
        }
    }));

    let client = Client::new("127.0.0.1", fake.port, "", Duration::from_secs(1))
        .expect("valid host must succeed");

    let status = client.get_status().expect("get status on valid host");
    assert_eq!(status.goroutines, 10);
    assert!(status.traffic_available);

    let recorded = fake.recorded_requests();
    assert_eq!(recorded.len(), 1);
    assert_eq!(recorded[0].path, "/daemon.StartedService/SubscribeStatus");
    assert_eq!(
        recorded[0].headers.get("host").map(|s| s.as_str()),
        Some(format!("127.0.0.1:{}", fake.port).as_str())
    );
}

// ============================================================================
// Ported Test: Mirrors tests/test_api_snapshot.c
// Snapshot generation progression, failed status invalidation, stale threshold.
// ============================================================================
#[test]
fn test_mirrors_test_api_snapshot() {
    let calls = Arc::new(Mutex::new(0usize));
    let calls_clone = Arc::clone(&calls);

    let fake = FakeSingbox::new(Arc::new(move |req| {
        if req.path == "/daemon.StartedService/SubscribeStatus" {
            let mut c = calls_clone.lock().expect("lock calls");
            *c += 1;
            let call_num = *c;
            if call_num == 2 {
                // Fail second status call as in test_api_snapshot.c
                None
            } else if call_num == 1 {
                Some(build_status_response(11, false))
            } else {
                Some(build_status_response(13, true))
            }
        } else if req.path == "/daemon.StartedService/GetVersion" {
            let c = *calls_clone.lock().expect("lock calls");
            Some(build_version_response(&format!("1.12.{}", c)))
        } else if req.path == "/daemon.StartedService/GetClashModeStatus" {
            Some(build_clash_mode_status_response(
                &["Rule", "Global", "Direct"],
                "Rule",
            ))
        } else {
            None
        }
    }));

    let mut reactor = Reactor::new().expect("create reactor");
    let waker = reactor.waker();
    let config = default_test_config(fake.port);
    let mut api = ApiHandle::new(&config, waker).expect("start api handle");

    // Stage 1: Round 1 succeeds
    let mut events = Vec::new();
    while api.snapshot().generation == 0 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
    }
    let snap1 = api.snapshot();
    assert!(snap1.valid);
    assert!(snap1.version_valid);
    assert!(snap1.clash_mode_valid);
    assert_eq!(snap1.generation, 1);
    assert_eq!(snap1.status.goroutines, 11);
    assert_eq!(snap1.version, "1.12.1");
    assert_eq!(snap1.clash_mode, "Rule");

    // Stage 2: Round 2 status call fails
    while api.snapshot().generation < 2 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
    }
    let snap2 = api.snapshot();
    assert!(!snap2.valid, "Failed status must invalidate snapshot");
    assert!(!snap2.version_valid);
    assert!(!snap2.clash_mode_valid);
    assert_eq!(snap2.generation, 2);

    // Stage 3: Round 3 succeeds with traffic available
    while api.snapshot().generation < 3 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
    }
    let snap3 = api.snapshot();
    assert!(snap3.valid);
    assert!(snap3.version_valid);
    assert!(snap3.clash_mode_valid);
    assert!(snap3.generation >= 3);
    assert_eq!(snap3.status.goroutines, 13);
    assert!(snap3.status.traffic_available);

    // Staleness check: API_SNAPSHOT_STALE_MS is 12000 ms.
    // 11000 ms ago: still valid.
    let now = crate::reactor::now_ms();
    let mut stale_candidate = snap3.clone();
    stale_candidate.updated_at_ms = now.saturating_sub(11_000);
    api.set_snapshot_for_test(stale_candidate);
    let s_fresh = api.snapshot();
    assert!(s_fresh.valid, "11s gap must still be fresh (< 12s budget)");

    // 13000 ms ago: exceeds 12s budget, must be marked invalid.
    let mut expired_candidate = snap3;
    expired_candidate.updated_at_ms = now.saturating_sub(13_000);
    api.set_snapshot_for_test(expired_candidate);
    let s_expired = api.snapshot();
    assert!(!s_expired.valid, "13s gap must exceed stale threshold");
    assert!(!s_expired.version_valid);
    assert!(!s_expired.clash_mode_valid);

    api.shutdown();
}

// ============================================================================
// Ported Test: Mirrors tests/test_api_vpn_mode.c
// Mode restore after teardown, case-insensitive mode matching, fallback mode.
// ============================================================================
#[test]
fn test_mirrors_test_api_vpn_mode() {
    let current_mode = Arc::new(Mutex::new("Global".to_string()));
    let last_set_mode = Arc::new(Mutex::new(String::new()));
    let set_calls = Arc::new(Mutex::new(0usize));

    let current_clone = Arc::clone(&current_mode);
    let last_set_clone = Arc::clone(&last_set_mode);
    let set_calls_clone = Arc::clone(&set_calls);

    let modes = [
        "Rule".to_string(),
        "Global".to_string(),
        "Direct".to_string(),
        "Google VPN".to_string(),
    ];

    let fake = FakeSingbox::new(Arc::new(move |req| {
        if req.path == "/daemon.StartedService/GetClashModeStatus" {
            let cur = current_clone.lock().expect("lock current").clone();
            let mode_refs: Vec<&str> = modes.iter().map(|s| s.as_str()).collect();
            Some(build_clash_mode_status_response(&mode_refs, &cur))
        } else if req.path == "/daemon.StartedService/SetClashMode" {
            let mut calls = set_calls_clone.lock().expect("lock calls");
            *calls += 1;
            if req.body.len() >= 5 {
                // Protobuf field 3: string mode = 3;
                let payload = &req.body[5..];
                if let Ok(m) = proto::parse_string_field(&req.body, 3) {
                    *current_clone.lock().expect("lock current") = m.clone();
                    *last_set_clone.lock().expect("lock last set") = m;
                } else if payload.len() >= 2 && payload[0] == 0x1a {
                    let len = payload[1] as usize;
                    if payload.len() >= 2 + len {
                        let m = std::str::from_utf8(&payload[2..2 + len]).unwrap_or("");
                        *current_clone.lock().expect("lock current") = m.to_string();
                        *last_set_clone.lock().expect("lock last set") = m.to_string();
                    }
                }
            }
            Some(build_empty_unary_response())
        } else if req.path == "/daemon.StartedService/SubscribeStatus" {
            Some(build_status_response(10, true))
        } else if req.path == "/daemon.StartedService/GetVersion" {
            Some(build_version_response("1.12.0"))
        } else {
            None
        }
    }));

    let mut reactor = Reactor::new().expect("create reactor");
    let mut config = default_test_config(fake.port);
    config.interface.vpn_auto_mode = true;
    config.interface.vpn_target_mode = "Google VPN".to_string();
    config.interface.vpn_fallback_mode = "Rule".to_string();

    let mut api = ApiHandle::new(&config, reactor.waker()).expect("start api");

    // Wait for first worker round to initialize
    let mut events = Vec::new();
    while api.snapshot().generation == 0 {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
    }

    // 1. VPN connects -> Ready. Mode switches to "Google VPN", prior mode "Global" saved
    api.on_vpn_change(VpnChange {
        state: VpnState::Ready,
        iface: Some("tun0".to_string()),
        teardown: false,
    });

    let start = Instant::now();
    while last_set_mode.lock().expect("lock").as_str() != "Google VPN"
        && start.elapsed() < Duration::from_secs(3)
    {
        events.clear();
        let _ = reactor.wait(&mut events);
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
        thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(last_set_mode.lock().expect("lock").as_str(), "Google VPN");

    // 2. VPN disconnects -> Idle. Prior mode "Global" restored, default_mode cleared
    api.on_vpn_change(VpnChange {
        state: VpnState::Idle,
        iface: None,
        teardown: false,
    });

    let start = Instant::now();
    while last_set_mode.lock().expect("lock").as_str() != "Global"
        && start.elapsed() < Duration::from_secs(3)
    {
        events.clear();
        let _ = reactor.wait(&mut events);
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
        thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(last_set_mode.lock().expect("lock").as_str(), "Global");

    // 3. Case-insensitive mode matching: "google vpn" matches "Google VPN"
    let status = SingboxClashModeStatus {
        modes: vec![
            "Rule".to_string(),
            "Global".to_string(),
            "Direct".to_string(),
            "Google VPN".to_string(),
        ],
        current_mode: "Global".to_string(),
    };
    assert_eq!(match_clash_mode(&status, "google vpn"), Some("Google VPN"));
    assert_eq!(match_clash_mode(&status, "GLOBAL"), Some("Global"));
    assert_eq!(match_clash_mode(&status, "direct"), Some("Direct"));
    assert_eq!(match_clash_mode(&status, "nonexistent"), None);
    assert!(is_clash_mode_supported(&status, "gOoGlE vPn"));
    assert!(!is_clash_mode_supported(&status, "random"));

    // 4. Mode restore after teardown edge (VpnChange with teardown=true)
    *current_mode.lock().expect("lock") = "Global".to_string();
    api.on_vpn_change(VpnChange {
        state: VpnState::Ready,
        iface: Some("tun0".to_string()),
        teardown: false,
    });
    let start = Instant::now();
    while last_set_mode.lock().expect("lock").as_str() != "Google VPN"
        && start.elapsed() < Duration::from_secs(3)
    {
        events.clear();
        let _ = reactor.wait(&mut events);
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
        thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(last_set_mode.lock().expect("lock").as_str(), "Google VPN");

    // Send teardown transition
    api.on_vpn_change(VpnChange {
        state: VpnState::Teardown,
        iface: None,
        teardown: true,
    });
    let start = Instant::now();
    while last_set_mode.lock().expect("lock").as_str() != "Global"
        && start.elapsed() < Duration::from_secs(3)
    {
        events.clear();
        let _ = reactor.wait(&mut events);
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
        thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(last_set_mode.lock().expect("lock").as_str(), "Global");

    api.shutdown();
}

// ============================================================================
// Acceptance Test: Worker delivers a snapshot via Waker to a real Reactor wait
// ============================================================================
#[test]
fn test_worker_delivers_snapshot_via_waker_to_real_reactor_wait() {
    let fake = FakeSingbox::new(Arc::new(|req| {
        if req.path == "/daemon.StartedService/SubscribeStatus" {
            Some(build_status_response(42, true))
        } else if req.path == "/daemon.StartedService/GetVersion" {
            Some(build_version_response("2.0.0"))
        } else if req.path == "/daemon.StartedService/GetClashModeStatus" {
            Some(build_clash_mode_status_response(
                &["Rule", "Global"],
                "Rule",
            ))
        } else {
            None
        }
    }));

    let mut reactor = Reactor::new().expect("create reactor");
    let config = default_test_config(fake.port);
    let mut api = ApiHandle::new(&config, reactor.waker()).expect("start api");

    let mut events = Vec::new();
    let mut received_wakeup = false;
    let start = Instant::now();

    while !received_wakeup && start.elapsed() < Duration::from_secs(3) {
        events.clear();
        reactor.wait(&mut events).expect("reactor wait");
        for ev in &events {
            if *ev == Event::Wakeup {
                received_wakeup = true;
                api.on_wakeup();
            }
        }
    }

    assert!(
        received_wakeup,
        "Reactor must receive Event::Wakeup from worker"
    );
    let snap = api.snapshot();
    assert!(snap.valid);
    assert_eq!(snap.status.goroutines, 42);
    assert_eq!(snap.version, "2.0.0");
    assert_eq!(snap.clash_mode, "Rule");

    api.shutdown();
}

// ============================================================================
// Integration Test: ApiProbes implements crate::supervisor::Probes
// ============================================================================
#[test]
fn test_supervisor_probes_integration() {
    let fake = FakeSingbox::new(Arc::new(|req| {
        if req.path == "/daemon.StartedService/SubscribeStatus" {
            Some(build_status_response(15, true))
        } else if req.path == "/daemon.StartedService/GetVersion" {
            Some(build_version_response("1.0.0"))
        } else if req.path == "/daemon.StartedService/GetClashModeStatus" {
            Some(build_clash_mode_status_response(&["Rule"], "Rule"))
        } else {
            None
        }
    }));

    let mut reactor = Reactor::new().expect("reactor");
    let config = default_test_config(fake.port);
    let mut api = ApiHandle::new(&config, reactor.waker()).expect("api");

    let mut events = Vec::new();
    while api.snapshot().generation == 0 {
        events.clear();
        let _ = reactor.wait(&mut events);
        for ev in &events {
            if *ev == Event::Wakeup {
                api.on_wakeup();
            }
        }
    }

    let mut probes = ApiProbes::new(&api).expect("api probes");

    // api_ready: calls SubscribeStatus with 1s timeout
    assert!(probes.api_ready());

    // api_healthy: checks snapshot validity
    assert!(probes.api_healthy());

    api.shutdown();
}
