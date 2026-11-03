//! UDS Server and client implementation for ATPD.
//!
//! Mirrors `src/uds.c`. Implements Unix Domain Socket command dispatch,
//! credentials checking via `SO_PEERCRED`, bounded non-blocking client I/O,
//! idle client timeout pruning, and command responses.

use std::io::{self, Read, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{FileTypeExt, PermissionsExt};
use std::os::unix::io::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::time::Duration;

use rustix::io::Errno;
use rustix::net::sockopt::{self, Timeout};

use crate::reactor::{Reactor, TimerId, EDGE, READ, WRITE};
use crate::status::StatusSnapshot;

pub const UDS_BUFFER_SIZE: usize = 4096;
pub const UDS_MAX_RESPONSE_SIZE: usize = 64 * 1024;
pub const UDS_PATH_MAX: usize = 108;
pub const UDS_MAX_CLIENTS: usize = 32;
pub const UDS_IDLE_TIMEOUT_MS: u64 = 5000;
pub const UDS_IDLE_SCAN_MS: u64 = 1000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IpcRequest {
    Shutdown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ClientState {
    Reading,
    Writing,
}

struct UdsClient {
    stream: UnixStream,
    token: u64,
    state: ClientState,
    input: [u8; UDS_BUFFER_SIZE],
    input_len: usize,
    output: Vec<u8>,
    output_off: usize,
    last_activity_ms: u64,
    stop_after_write: bool,
}

pub struct IpcServer {
    listener: Option<UnixListener>,
    path: PathBuf,
    allowed_uid: u32,
    listen_token: u64,
    client_token_base: u64,
    next_client_token: u64,
    idle_timer_id: Option<TimerId>,
    idle_timeout_ms: u64,
    clients: Vec<UdsClient>,
}

fn check_client_uid(stream: &UnixStream, allowed_uid: u32) -> bool {
    match sockopt::socket_peercred(stream) {
        Ok(cred) => cred.uid.is_root() || cred.uid.as_raw() == allowed_uid,
        Err(_) => false,
    }
}

fn check_path_len(path: &Path) -> io::Result<()> {
    if path.as_os_str().as_bytes().len() >= UDS_PATH_MAX {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "socket path too long",
        ));
    }
    Ok(())
}

fn prepare_socket_path(path: &Path) -> io::Result<()> {
    check_path_len(path)?;

    match std::fs::symlink_metadata(path) {
        Ok(meta) => {
            if !meta.file_type().is_socket() {
                return Err(io::Error::new(
                    io::ErrorKind::AlreadyExists,
                    "refusing to replace non-socket path",
                ));
            }
            // Probe existing socket; only a dead one may be replaced.
            match UnixStream::connect(path) {
                Ok(_) => {
                    return Err(io::Error::new(
                        io::ErrorKind::AddrInUse,
                        "active socket already exists",
                    ))
                }
                Err(e)
                    if e.raw_os_error() == Some(libc::ECONNREFUSED)
                        || e.raw_os_error() == Some(libc::ENOENT) => {}
                Err(e) => return Err(e),
            }
            let _ = std::fs::remove_file(path);
        }
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(e),
    }

    Ok(())
}

impl IpcServer {
    pub fn new(
        path: impl AsRef<Path>,
        allowed_uid: u32,
        listen_token: u64,
        client_token_base: u64,
        reactor: &mut Reactor,
    ) -> io::Result<Self> {
        let path = path.as_ref().to_path_buf();
        prepare_socket_path(&path)?;

        let listener = UnixListener::bind(&path)?;
        let setup = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))
            .and_then(|_| listener.set_nonblocking(true))
            .and_then(|_| reactor.add_fd(listener.as_raw_fd(), READ | EDGE, listen_token));
        if let Err(e) = setup {
            let _ = std::fs::remove_file(&path);
            return Err(e);
        }

        let timer_id = reactor.add_timer(UDS_IDLE_SCAN_MS, UDS_IDLE_SCAN_MS);

        Ok(Self {
            listener: Some(listener),
            path,
            allowed_uid,
            listen_token,
            client_token_base,
            next_client_token: client_token_base,
            idle_timer_id: Some(timer_id),
            idle_timeout_ms: UDS_IDLE_TIMEOUT_MS,
            clients: Vec::new(),
        })
    }
    pub fn listen_token(&self) -> u64 {
        self.listen_token
    }

    fn allocate_client_token(&mut self) -> u64 {
        let token = self.next_client_token;
        self.next_client_token = self.next_client_token.wrapping_add(1);
        if self.next_client_token < self.client_token_base {
            self.next_client_token = self.client_token_base;
        }
        token
    }

    pub fn check_timeouts(&mut self, r: &mut Reactor) {
        let now = Reactor::now_ms();
        let timeout = self.idle_timeout_ms;
        self.clients.retain(|c| {
            let keep = now.saturating_sub(c.last_activity_ms) < timeout;
            if !keep {
                let _ = r.remove_fd(c.stream.as_raw_fd());
            }
            keep
        });
    }

    pub fn handle_timer(&mut self, r: &mut Reactor, timer_id: TimerId) -> bool {
        if self.idle_timer_id == Some(timer_id) {
            self.check_timeouts(r);
            true
        } else {
            false
        }
    }

    fn queue_response(client: &mut UdsClient, data: &[u8]) {
        if client.output.len() + data.len() > UDS_MAX_RESPONSE_SIZE {
            let err_msg = b"ERROR: response too large\n";
            client.output = err_msg.to_vec();
            client.output_off = 0;
            return;
        }
        client.output.extend_from_slice(data);
    }

    fn process_command(
        client: &mut UdsClient,
        cmd_bytes: &[u8],
        status: &dyn Fn() -> StatusSnapshot,
        stats: crate::reactor::ReactorStats,
    ) {
        if cmd_bytes.len() >= UDS_BUFFER_SIZE - 1 {
            Self::queue_response(client, b"ERROR: command too long\n");
            return;
        }

        let cmd_str = match std::str::from_utf8(cmd_bytes) {
            Ok(s) => s,
            Err(_) => {
                Self::queue_response(
                    client,
                    b"Unknown command. Type 'help' for available commands.\n",
                );
                return;
            }
        };

        let trimmed = cmd_str.trim();
        if trimmed.is_empty() {
            return;
        }

        if trimmed == "status" {
            let snap = status();
            let text = crate::ui::dashboard::status_render_to_string(&snap, 0, true);
            Self::queue_response(client, text.as_bytes());
        } else if let Some(width_str) = trimmed.strip_prefix("status ") {
            let width_str = width_str.trim();
            if let Ok(width) = width_str.parse::<usize>() {
                if (30..=200).contains(&width) {
                    let snap = status();
                    let text = crate::ui::dashboard::status_render_to_string(&snap, width, true);
                    Self::queue_response(client, text.as_bytes());
                } else {
                    Self::queue_response(client, b"ERROR: invalid status width\n");
                }
            } else {
                Self::queue_response(client, b"ERROR: invalid status width\n");
            }
        } else if trimmed == "status-summary" {
            let snap = status();
            let text = crate::ui::dashboard::status_summary_to_string(&snap);
            Self::queue_response(client, text.as_bytes());
        } else if trimmed == "stop" {
            Self::queue_response(client, b"Stopping ATPd...\n");
            client.stop_after_write = true;
        } else if trimmed == "ping" {
            Self::queue_response(client, b"pong\n");
        } else if trimmed == "version" {
            let resp = format!(
                "ATPd Version: {}\nGit Commit: {}\n",
                crate::version::get_full_version(),
                crate::version::get_commit()
            );
            Self::queue_response(client, resp.as_bytes());
        } else if trimmed == "stats" {
            let resp = format!(
                "=== Statistics ===\nEvents Processed: {}\nTimers Fired: {}\nSignals Received: {}\n",
                stats.events_processed, stats.timers_fired, stats.signals_received
            );
            Self::queue_response(client, resp.as_bytes());
        } else if trimmed == "help" || trimmed == "?" {
            let help = "Available commands:\n  status          - Show runtime status\n  status-summary  - Show concise runtime status\n  stop            - Shutdown ATPd\n  ping      - Check if ATPd is alive\n  version   - Show version information\n  stats     - Show statistics\n  help      - Show this help\n";
            Self::queue_response(client, help.as_bytes());
        } else {
            Self::queue_response(
                client,
                b"Unknown command. Type 'help' for available commands.\n",
            );
        }
    }

    fn flush_client(client: &mut UdsClient, r: &mut Reactor, now: u64) -> Result<bool, ()> {
        while client.output_off < client.output.len() {
            let to_send = &client.output[client.output_off..];
            match rustix::net::send(&client.stream, to_send, rustix::net::SendFlags::NOSIGNAL) {
                Ok(n) if n > 0 => {
                    client.output_off += n;
                    client.last_activity_ms = now;
                }
                Err(Errno::INTR) => {}
                Err(Errno::AGAIN) => {
                    let _ = r.modify_fd(client.stream.as_raw_fd(), WRITE | EDGE);
                    return Ok(false);
                }
                _ => return Err(()),
            }
        }
        Ok(true)
    }

    pub fn handle_io(
        &mut self,
        r: &mut Reactor,
        token: u64,
        status: &dyn Fn() -> StatusSnapshot,
    ) -> Vec<IpcRequest> {
        let mut requests = Vec::new();
        self.check_timeouts(r);
        let now = Reactor::now_ms();

        if token == self.listen_token {
            while let Some(accepted) = self.listener.as_ref().map(|l| l.accept()) {
                let stream = match accepted {
                    Ok((stream, _)) => stream,
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    Err(_) => break,
                };

                // Dropping a rejected stream closes it.
                if !check_client_uid(&stream, self.allowed_uid)
                    || self.clients.len() >= UDS_MAX_CLIENTS
                    || stream.set_nonblocking(true).is_err()
                {
                    continue;
                }

                let client_token = self.allocate_client_token();
                if r.add_fd(stream.as_raw_fd(), READ | EDGE, client_token)
                    .is_err()
                {
                    continue;
                }

                self.clients.push(UdsClient {
                    stream,
                    token: client_token,
                    state: ClientState::Reading,
                    input: [0; UDS_BUFFER_SIZE],
                    input_len: 0,
                    output: Vec::new(),
                    output_off: 0,
                    last_activity_ms: now,
                    stop_after_write: false,
                });
            }
            return requests;
        }

        let client_idx = match self.clients.iter().position(|c| c.token == token) {
            Some(idx) => idx,
            None => return requests,
        };

        let mut should_close = false;
        {
            let client = &mut self.clients[client_idx];

            if client.state == ClientState::Writing {
                match Self::flush_client(client, r, now) {
                    Ok(true) => {
                        if client.stop_after_write {
                            requests.push(IpcRequest::Shutdown);
                        }
                        should_close = true;
                    }
                    Ok(false) => {}
                    Err(()) => {
                        should_close = true;
                    }
                }
            } else {
                loop {
                    let space = client.input.len().saturating_sub(client.input_len + 1);
                    if space == 0 {
                        Self::queue_response(client, b"ERROR: command too long\n");
                        client.state = ClientState::Writing;
                        let _ = r.modify_fd(client.stream.as_raw_fd(), WRITE | EDGE);
                        break;
                    }

                    let end = client.input_len + space;
                    match client.stream.read(&mut client.input[client.input_len..end]) {
                        Ok(0) => {
                            if !client.input[..client.input_len].contains(&b'\n') {
                                should_close = true;
                            }
                            break;
                        }
                        Ok(n) => {
                            client.input_len += n;
                            client.last_activity_ms = now;
                            if client.input[..client.input_len].contains(&b'\n') {
                                break;
                            }
                        }
                        Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                        Err(_) => {
                            should_close = true;
                            break;
                        }
                    }
                }

                if !should_close {
                    if let Some(pos) = client.input[..client.input_len]
                        .iter()
                        .position(|&b| b == b'\n')
                    {
                        let cmd = client.input[..pos].to_vec();
                        Self::process_command(client, &cmd, status, r.stats());
                        client.state = ClientState::Writing;
                        let _ = r.modify_fd(client.stream.as_raw_fd(), WRITE | EDGE);
                    }

                    if client.state == ClientState::Writing {
                        match Self::flush_client(client, r, now) {
                            Ok(true) => {
                                if client.stop_after_write {
                                    requests.push(IpcRequest::Shutdown);
                                }
                                should_close = true;
                            }
                            Ok(false) => {}
                            Err(()) => {
                                should_close = true;
                            }
                        }
                    }
                }
            }
        }

        if should_close {
            let mut client = self.clients.swap_remove(client_idx);
            let _ = r.remove_fd(client.stream.as_raw_fd());
            // Drain unread bytes so kernel sends FIN rather than RST on close
            let mut drain_buf = [0u8; 4096];
            while matches!(client.stream.read(&mut drain_buf), Ok(n) if n > 0) {}
        }

        requests
    }

    pub fn cleanup(&mut self, r: &mut Reactor) {
        if let Some(id) = self.idle_timer_id.take() {
            r.cancel_timer(id);
        }
        for client in self.clients.drain(..) {
            let _ = r.remove_fd(client.stream.as_raw_fd());
        }
        if let Some(listener) = self.listener.take() {
            let _ = r.remove_fd(listener.as_raw_fd());
        }
        let _ = std::fs::remove_file(&self.path);
    }
}

impl Drop for IpcServer {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

pub fn query_daemon(path: impl AsRef<Path>, command: &str) -> io::Result<String> {
    let path = path.as_ref();
    check_path_len(path)?;
    let addr = rustix::net::SocketAddrUnix::new(path)?;
    let timeout = Some(Duration::from_secs(3));

    let mut stream = None;
    for retry in 0..10 {
        let fd = rustix::net::socket_with(
            rustix::net::AddressFamily::UNIX,
            rustix::net::SocketType::STREAM,
            rustix::net::SocketFlags::CLOEXEC,
            None,
        )?;
        // Set before connect so a full backlog times out instead of blocking forever.
        let _ = sockopt::set_socket_timeout(&fd, Timeout::Recv, timeout);
        let _ = sockopt::set_socket_timeout(&fd, Timeout::Send, timeout);

        match rustix::net::connect(&fd, &addr) {
            Ok(()) => {
                stream = Some(UnixStream::from(fd));
                break;
            }
            Err(e @ (Errno::NOENT | Errno::CONNREFUSED)) => return Err(e.into()),
            Err(_) if retry < 9 => std::thread::sleep(Duration::from_millis(50)),
            Err(_) => {}
        }
    }
    let mut stream = stream.ok_or_else(|| {
        io::Error::new(io::ErrorKind::NotConnected, "failed to connect to daemon")
    })?;

    stream.write_all(command.as_bytes())?;
    let mut response = Vec::new();
    stream.read_to_end(&mut response)?;
    String::from_utf8(response).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::reactor::Event;
    use std::os::unix::fs::PermissionsExt;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static TEST_COUNTER: AtomicUsize = AtomicUsize::new(1);

    fn test_sock_path() -> PathBuf {
        let count = TEST_COUNTER.fetch_add(1, Ordering::SeqCst);
        std::env::temp_dir().join(format!("atpd_test_{}_{}.sock", std::process::id(), count))
    }

    fn run_server_for_one_req(
        reactor: &mut Reactor,
        server: &mut IpcServer,
        status: &dyn Fn() -> StatusSnapshot,
        max_events: usize,
    ) -> Vec<IpcRequest> {
        let mut all_reqs = Vec::new();
        let mut events = Vec::new();
        let timeout_timer = reactor.add_timer(500, 0);
        for _ in 0..max_events {
            events.clear();
            if reactor.wait(&mut events).is_err() {
                break;
            }
            if events.is_empty() {
                break;
            }
            let mut timed_out = false;
            for ev in &events {
                match *ev {
                    Event::Io { token, .. } => {
                        let reqs = server.handle_io(reactor, token, status);
                        all_reqs.extend(reqs);
                    }
                    Event::Timer(id) => {
                        if id == timeout_timer {
                            timed_out = true;
                        }
                        server.handle_timer(reactor, id);
                    }
                    _ => {}
                }
            }
            if timed_out {
                break;
            }
            if server.clients.is_empty() && !all_reqs.is_empty() {
                break;
            }
        }
        reactor.cancel_timer(timeout_timer);
        all_reqs
    }

    #[test]
    fn test_ipc_socket_file_mode_0600() {
        let mut reactor = Reactor::new().unwrap();
        let sock_path = test_sock_path();
        let allowed_uid = rustix::process::geteuid().as_raw();
        let mut server = IpcServer::new(&sock_path, allowed_uid, 10, 100, &mut reactor).unwrap();

        let meta = std::fs::metadata(&sock_path).unwrap();
        let mode = meta.permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "socket file mode must be 0600");

        server.cleanup(&mut reactor);
    }

    #[test]
    fn test_ipc_status_request_round_trip() {
        let mut reactor = Reactor::new().unwrap();
        let sock_path = test_sock_path();
        let allowed_uid = rustix::process::geteuid().as_raw();
        let mut server = IpcServer::new(&sock_path, allowed_uid, 10, 100, &mut reactor).unwrap();

        let dummy_status = || StatusSnapshot {
            daemon_running: true,
            atpd_pid: 9999,
            ..Default::default()
        };

        let path_clone = sock_path.clone();
        let client_thread = std::thread::spawn(move || query_daemon(&path_clone, "status\n"));

        // Run reactor until request is handled
        run_server_for_one_req(&mut reactor, &mut server, &dummy_status, 10);

        let res = client_thread.join().unwrap().unwrap();
        assert!(res.contains("ATPD DAEMON"));
        assert!(res.contains("9999"));

        server.cleanup(&mut reactor);
    }

    #[test]
    fn test_ipc_overlong_request_rejected() {
        let mut reactor = Reactor::new().unwrap();
        let sock_path = test_sock_path();
        let allowed_uid = rustix::process::geteuid().as_raw();
        let mut server = IpcServer::new(&sock_path, allowed_uid, 10, 100, &mut reactor).unwrap();

        let dummy_status = StatusSnapshot::default;

        let path_clone = sock_path.clone();
        let client_thread = std::thread::spawn(move || {
            let overlong = "A".repeat(5000);
            query_daemon(&path_clone, &overlong)
        });

        run_server_for_one_req(&mut reactor, &mut server, &dummy_status, 10);

        let res = client_thread.join().unwrap().unwrap();
        assert!(res.contains("ERROR: command too long"));

        server.cleanup(&mut reactor);
    }

    #[test]
    fn test_ipc_disallowed_uid_rejected() {
        let mut reactor = Reactor::new().unwrap();
        let sock_path = test_sock_path();
        // Disallow current user
        let disallowed_uid = rustix::process::geteuid().as_raw() + 1;
        let mut server = IpcServer::new(&sock_path, disallowed_uid, 10, 100, &mut reactor).unwrap();

        let dummy_status = StatusSnapshot::default;

        let path_clone = sock_path.clone();
        let client_thread = std::thread::spawn(move || query_daemon(&path_clone, "ping\n"));

        run_server_for_one_req(&mut reactor, &mut server, &dummy_status, 5);

        let res = client_thread.join().unwrap();
        // Client should receive empty response or connection refused / broken pipe
        assert!(res.is_err() || res.unwrap().is_empty());

        server.cleanup(&mut reactor);
    }

    #[test]
    fn test_ipc_silent_client_timeout_and_non_blocking() {
        let mut reactor = Reactor::new().unwrap();
        let sock_path = test_sock_path();
        let allowed_uid = rustix::process::geteuid().as_raw();
        let mut server = IpcServer::new(&sock_path, allowed_uid, 10, 100, &mut reactor).unwrap();

        // Set short timeout for test
        server.idle_timeout_ms = 150;

        let dummy_status = StatusSnapshot::default;

        // 1. Silent client connects and sends nothing
        let silent = UnixStream::connect(&sock_path).unwrap();

        // Reactor accepts silent client
        let mut events = Vec::new();
        let t_accept = reactor.add_timer(200, 0);
        reactor.wait(&mut events).unwrap();
        reactor.cancel_timer(t_accept);
        for ev in &events {
            if let Event::Io { token, .. } = *ev {
                server.handle_io(&mut reactor, token, &dummy_status);
            }
        }
        assert_eq!(server.clients.len(), 1);

        // 2. Active client connects and sends ping; must not be blocked by silent client
        let path_clone = sock_path.clone();
        let active_thread = std::thread::spawn(move || query_daemon(&path_clone, "ping\n"));

        // Run reactor to handle active client
        for _ in 0..10 {
            events.clear();
            let t_active = reactor.add_timer(200, 0);
            if reactor.wait(&mut events).is_err() {
                break;
            }
            reactor.cancel_timer(t_active);
            for ev in &events {
                if let Event::Io { token, .. } = *ev {
                    server.handle_io(&mut reactor, token, &dummy_status);
                }
            }
            if active_thread.is_finished() {
                break;
            }
        }

        let pong = active_thread.join().unwrap().unwrap();
        assert_eq!(pong, "pong\n");

        // 3. Wait for idle timeout to expire
        std::thread::sleep(std::time::Duration::from_millis(160));
        server.check_timeouts(&mut reactor);
        assert_eq!(
            server.clients.len(),
            0,
            "silent client must be dropped after idle timeout"
        );

        drop(silent);
        server.cleanup(&mut reactor);
    }
}
