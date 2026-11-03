//! UDS Server and client implementation for ATPD.
//!
//! Mirrors `src/uds.c`. Implements Unix Domain Socket command dispatch,
//! credentials checking via `SO_PEERCRED`, bounded non-blocking client I/O,
//! idle client timeout pruning, and command responses.

use std::io;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::RawFd;
use std::path::{Path, PathBuf};

use crate::reactor::{Reactor, TimerId, EDGE, READ, WRITE};
use crate::status::StatusSnapshot;

pub const UDS_BUFFER_SIZE: usize = 4096;
pub const UDS_RESPONSE_SIZE: usize = 8192;
pub const UDS_MAX_RESPONSE_SIZE: usize = 64 * 1024;
pub const UDS_BACKLOG: i32 = 16;
pub const UDS_PATH_MAX: usize = 108;
pub const UDS_MAX_CLIENTS: usize = 32;
pub const UDS_IDLE_TIMEOUT_MS: u64 = 5000;
pub const UDS_IDLE_SCAN_MS: u64 = 1000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IpcRequest {
    Shutdown,
    Reload,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ClientState {
    Reading,
    Writing,
}

struct UdsClient {
    fd: RawFd,
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
    listen_fd: RawFd,
    path: PathBuf,
    allowed_uid: u32,
    listen_token: u64,
    client_token_base: u64,
    next_client_token: u64,
    idle_timer_id: Option<TimerId>,
    idle_timeout_ms: u64,
    clients: Vec<UdsClient>,
}

fn check_client_uid(fd: RawFd, allowed_uid: u32) -> bool {
    let mut cred = libc::ucred {
        pid: 0,
        uid: 0,
        gid: 0,
    };
    let mut len = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
    // SAFETY: getsockopt with SO_PEERCRED writes valid ucred struct.
    let ret = unsafe {
        libc::getsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            &mut cred as *mut _ as *mut libc::c_void,
            &mut len,
        )
    };
    if ret < 0 {
        return false;
    }
    cred.uid == 0 || cred.uid == allowed_uid
}

fn prepare_socket_path(path: &Path) -> io::Result<()> {
    let path_bytes = path.as_os_str().as_bytes();
    if path_bytes.len() >= UDS_PATH_MAX {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "socket path too long",
        ));
    }

    match std::fs::symlink_metadata(path) {
        Ok(meta) => {
            let file_type = meta.file_type();
            #[cfg(unix)]
            {
                use std::os::unix::fs::FileTypeExt;
                if !file_type.is_socket() {
                    return Err(io::Error::new(
                        io::ErrorKind::AlreadyExists,
                        "refusing to replace non-socket path",
                    ));
                }
            }

            // Probe existing socket
            let probe =
                // SAFETY: socket creation for probing unix stream socket.
                unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0) };
            if probe >= 0 {
                let mut addr = std::mem::MaybeUninit::<libc::sockaddr_un>::uninit();
                // SAFETY: zero out sockaddr_un.
                unsafe {
                    libc::memset(
                        addr.as_mut_ptr() as *mut libc::c_void,
                        0,
                        std::mem::size_of::<libc::sockaddr_un>(),
                    )
                };
                // SAFETY: addr was zero-initialized.
                let mut addr = unsafe { addr.assume_init() };
                addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
                for (dst, &src) in addr.sun_path.iter_mut().zip(path_bytes.iter()) {
                    *dst = src as libc::c_char;
                }

                // SAFETY: connect to probe address.
                let ret = unsafe {
                    libc::connect(
                        probe,
                        &addr as *const _ as *const libc::sockaddr,
                        std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
                    )
                };
                if ret == 0 {
                    // SAFETY: close probe descriptor.
                    unsafe { libc::close(probe) };
                    return Err(io::Error::new(
                        io::ErrorKind::AddrInUse,
                        "active socket already exists",
                    ));
                }
                let err = io::Error::last_os_error();
                // SAFETY: close probe descriptor.
                unsafe { libc::close(probe) };
                if err.raw_os_error() != Some(libc::ECONNREFUSED)
                    && err.raw_os_error() != Some(libc::ENOENT)
                {
                    return Err(err);
                }
            }

            // Stale socket, remove it
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

        let path_bytes = path.as_os_str().as_bytes();

        // SAFETY: socket create non-blocking close-on-exec unix stream socket.
        let fd = unsafe {
            libc::socket(
                libc::AF_UNIX,
                libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
                0,
            )
        };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut addr = std::mem::MaybeUninit::<libc::sockaddr_un>::uninit();
        // SAFETY: zero out sockaddr_un.
        unsafe {
            libc::memset(
                addr.as_mut_ptr() as *mut libc::c_void,
                0,
                std::mem::size_of::<libc::sockaddr_un>(),
            )
        };
        // SAFETY: addr was zero-initialized.
        let mut addr = unsafe { addr.assume_init() };
        addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
        for (dst, &src) in addr.sun_path.iter_mut().zip(path_bytes.iter()) {
            *dst = src as libc::c_char;
        }

        // SAFETY: bind socket descriptor to sun_path.
        let ret = unsafe {
            libc::bind(
                fd,
                &addr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
            )
        };
        if ret < 0 {
            let err = io::Error::last_os_error();
            // SAFETY: close descriptor on failure.
            unsafe { libc::close(fd) };
            return Err(err);
        }

        let c_path = std::ffi::CString::new(path_bytes)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
        // SAFETY: chmod 0600 on created socket path.
        let chmod_ret = unsafe { libc::chmod(c_path.as_ptr(), 0o600) };
        if chmod_ret < 0 {
            let err = io::Error::last_os_error();
            // SAFETY: close descriptor on chmod failure.
            unsafe { libc::close(fd) };
            let _ = std::fs::remove_file(&path);
            return Err(err);
        }

        // SAFETY: listen on socket descriptor with backlog 16.
        let listen_ret = unsafe { libc::listen(fd, UDS_BACKLOG) };
        if listen_ret < 0 {
            let err = io::Error::last_os_error();
            // SAFETY: close descriptor on listen failure.
            unsafe { libc::close(fd) };
            let _ = std::fs::remove_file(&path);
            return Err(err);
        }

        if let Err(e) = reactor.add_fd(fd, READ | EDGE, listen_token) {
            // SAFETY: close descriptor on reactor add failure.
            unsafe { libc::close(fd) };
            let _ = std::fs::remove_file(&path);
            return Err(e);
        }

        let timer_id = reactor.add_timer(UDS_IDLE_SCAN_MS, UDS_IDLE_SCAN_MS);

        Ok(Self {
            listen_fd: fd,
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

    pub fn set_idle_timeout_ms(&mut self, timeout_ms: u64) {
        self.idle_timeout_ms = timeout_ms;
    }

    pub fn listen_token(&self) -> u64 {
        self.listen_token
    }

    pub fn active_clients(&self) -> usize {
        self.clients.len()
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
        let mut to_drop = Vec::new();
        for (i, c) in self.clients.iter().enumerate() {
            if now.saturating_sub(c.last_activity_ms) >= self.idle_timeout_ms {
                to_drop.push(i);
            }
        }
        for &idx in to_drop.iter().rev() {
            let client = self.clients.swap_remove(idx);
            let _ = r.remove_fd(client.fd);
            // SAFETY: close timed out client fd.
            unsafe { libc::close(client.fd) };
        }
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
            // SAFETY: send output bytes to connected non-blocking socket with MSG_NOSIGNAL.
            let n = unsafe {
                libc::send(
                    client.fd,
                    to_send.as_ptr() as *const libc::c_void,
                    to_send.len(),
                    libc::MSG_NOSIGNAL,
                )
            };
            if n > 0 {
                client.output_off += n as usize;
                client.last_activity_ms = now;
                continue;
            }
            if n < 0 {
                let err = io::Error::last_os_error();
                if err.raw_os_error() == Some(libc::EINTR) {
                    continue;
                }
                if err.raw_os_error() == Some(libc::EAGAIN)
                    || err.raw_os_error() == Some(libc::EWOULDBLOCK)
                {
                    let _ = r.modify_fd(client.fd, WRITE | EDGE);
                    return Ok(false);
                }
            }
            return Err(());
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
            loop {
                // SAFETY: accept4 with non-blocking close-on-exec.
                let client_fd = unsafe {
                    libc::accept4(
                        self.listen_fd,
                        std::ptr::null_mut(),
                        std::ptr::null_mut(),
                        libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
                    )
                };
                if client_fd < 0 {
                    let err = io::Error::last_os_error();
                    if err.raw_os_error() == Some(libc::EINTR) {
                        continue;
                    }
                    break;
                }

                if !check_client_uid(client_fd, self.allowed_uid)
                    || self.clients.len() >= UDS_MAX_CLIENTS
                {
                    // SAFETY: close rejected client descriptor.
                    unsafe { libc::close(client_fd) };
                    continue;
                }

                let client_token = self.allocate_client_token();
                if r.add_fd(client_fd, READ | EDGE, client_token).is_err() {
                    // SAFETY: close descriptor if registration fails.
                    unsafe { libc::close(client_fd) };
                    continue;
                }

                self.clients.push(UdsClient {
                    fd: client_fd,
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
            let client = match self.clients.get_mut(client_idx) {
                Some(c) => c,
                None => return requests,
            };

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
                        let _ = r.modify_fd(client.fd, WRITE | EDGE);
                        break;
                    }

                    // SAFETY: read input bytes from client socket.
                    let n = unsafe {
                        libc::read(
                            client.fd,
                            client.input.as_mut_ptr().add(client.input_len) as *mut libc::c_void,
                            space,
                        )
                    };
                    if n > 0 {
                        client.input_len += n as usize;
                        client.last_activity_ms = now;
                        if client.input[..client.input_len].contains(&b'\n') {
                            break;
                        }
                        continue;
                    }
                    if n == 0 {
                        if !client.input[..client.input_len].contains(&b'\n') {
                            should_close = true;
                        }
                        break;
                    }
                    let err = io::Error::last_os_error();
                    if err.raw_os_error() == Some(libc::EINTR) {
                        continue;
                    }
                    if err.raw_os_error() == Some(libc::EAGAIN)
                        || err.raw_os_error() == Some(libc::EWOULDBLOCK)
                    {
                        break;
                    }
                    should_close = true;
                    break;
                }

                if !should_close {
                    if let Some(pos) = client.input[..client.input_len]
                        .iter()
                        .position(|&b| b == b'\n')
                    {
                        let cmd = client.input[..pos].to_vec();
                        Self::process_command(client, &cmd, status, r.stats());
                        client.state = ClientState::Writing;
                        let _ = r.modify_fd(client.fd, WRITE | EDGE);
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

        if should_close && client_idx < self.clients.len() {
            let client = self.clients.swap_remove(client_idx);
            let _ = r.remove_fd(client.fd);
            // Drain unread bytes so kernel sends FIN rather than RST on close
            let mut drain_buf = [0u8; 4096];
            loop {
                // SAFETY: drain unread bytes from socket.
                let n = unsafe {
                    libc::read(
                        client.fd,
                        drain_buf.as_mut_ptr() as *mut libc::c_void,
                        drain_buf.len(),
                    )
                };
                if n <= 0 {
                    break;
                }
            }
            // SAFETY: close client descriptor after session end.
            unsafe { libc::close(client.fd) };
        }

        requests
    }

    pub fn cleanup(&mut self, r: &mut Reactor) {
        if let Some(id) = self.idle_timer_id.take() {
            r.cancel_timer(id);
        }
        for client in self.clients.drain(..) {
            let _ = r.remove_fd(client.fd);
            // SAFETY: close client descriptor during cleanup.
            unsafe { libc::close(client.fd) };
        }
        if self.listen_fd >= 0 {
            let _ = r.remove_fd(self.listen_fd);
            // SAFETY: close listen descriptor during cleanup.
            unsafe { libc::close(self.listen_fd) };
            self.listen_fd = -1;
        }
        let _ = std::fs::remove_file(&self.path);
    }
}

impl Drop for IpcServer {
    fn drop(&mut self) {
        for client in self.clients.drain(..) {
            // SAFETY: close client descriptor in drop.
            unsafe { libc::close(client.fd) };
        }
        if self.listen_fd >= 0 {
            // SAFETY: close listen descriptor in drop.
            unsafe { libc::close(self.listen_fd) };
            self.listen_fd = -1;
        }
        let _ = std::fs::remove_file(&self.path);
    }
}

pub fn query_daemon(path: impl AsRef<Path>, command: &str) -> io::Result<String> {
    let path = path.as_ref();
    let path_bytes = path.as_os_str().as_bytes();
    if path_bytes.len() >= UDS_PATH_MAX {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "socket path too long",
        ));
    }

    let mut addr = std::mem::MaybeUninit::<libc::sockaddr_un>::uninit();
    // SAFETY: zero out sockaddr_un.
    unsafe {
        libc::memset(
            addr.as_mut_ptr() as *mut libc::c_void,
            0,
            std::mem::size_of::<libc::sockaddr_un>(),
        )
    };
    // SAFETY: addr was zero-initialized.
    let mut addr = unsafe { addr.assume_init() };
    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
    for (dst, &src) in addr.sun_path.iter_mut().zip(path_bytes.iter()) {
        *dst = src as libc::c_char;
    }

    let tv = libc::timeval {
        tv_sec: 3,
        tv_usec: 0,
    };

    let mut fd = -1;
    for retry in 0..10 {
        // SAFETY: socket create close-on-exec unix stream socket.
        fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }

        // SAFETY: setsockopt SO_RCVTIMEO with valid timeval.
        let _ = unsafe {
            libc::setsockopt(
                fd,
                libc::SOL_SOCKET,
                libc::SO_RCVTIMEO,
                &tv as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::timeval>() as libc::socklen_t,
            )
        };
        // SAFETY: setsockopt SO_SNDTIMEO with valid timeval.
        let _ = unsafe {
            libc::setsockopt(
                fd,
                libc::SOL_SOCKET,
                libc::SO_SNDTIMEO,
                &tv as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::timeval>() as libc::socklen_t,
            )
        };

        // SAFETY: connect to target unix socket.
        let ret = unsafe {
            libc::connect(
                fd,
                &addr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
            )
        };
        if ret == 0 {
            break;
        }
        let err = io::Error::last_os_error();
        // SAFETY: close fd on connect failure.
        unsafe { libc::close(fd) };
        fd = -1;
        if err.raw_os_error() == Some(libc::ENOENT)
            || err.raw_os_error() == Some(libc::ECONNREFUSED)
        {
            return Err(err);
        }
        if retry < 9 {
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
    }

    if fd < 0 {
        return Err(io::Error::new(
            io::ErrorKind::NotConnected,
            "failed to connect to daemon",
        ));
    }

    let cmd_bytes = command.as_bytes();
    let mut sent = 0;
    while sent < cmd_bytes.len() {
        // SAFETY: sending command to connected socket.
        let n = unsafe {
            libc::send(
                fd,
                cmd_bytes[sent..].as_ptr() as *const libc::c_void,
                cmd_bytes.len() - sent,
                libc::MSG_NOSIGNAL,
            )
        };
        if n > 0 {
            sent += n as usize;
            continue;
        }
        if n < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            // SAFETY: close fd on send failure.
            unsafe { libc::close(fd) };
            return Err(err);
        }
        break;
    }

    let mut response = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        // SAFETY: recv response from daemon socket.
        let n = unsafe { libc::recv(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len(), 0) };
        if n > 0 {
            response.extend_from_slice(&buf[..n as usize]);
            continue;
        }
        if n < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            // SAFETY: close fd on error.
            unsafe { libc::close(fd) };
            return Err(err);
        }
        break;
    }

    // SAFETY: close socket after communication.
    unsafe { libc::close(fd) };
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
            if server.active_clients() == 0 && !all_reqs.is_empty() {
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
        // SAFETY: geteuid syscall in test.
        let allowed_uid = unsafe { libc::geteuid() };
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
        // SAFETY: geteuid syscall in test.
        let allowed_uid = unsafe { libc::geteuid() };
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
        // SAFETY: geteuid syscall in test.
        let allowed_uid = unsafe { libc::geteuid() };
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
        // SAFETY: geteuid syscall in test.
        let disallowed_uid = unsafe { libc::geteuid() } + 1;
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
        // SAFETY: geteuid syscall in test.
        let allowed_uid = unsafe { libc::geteuid() };
        let mut server = IpcServer::new(&sock_path, allowed_uid, 10, 100, &mut reactor).unwrap();

        // Set short timeout for test
        server.set_idle_timeout_ms(150);

        let dummy_status = StatusSnapshot::default;

        // 1. Silent client connects and sends nothing
        // SAFETY: socket creation for silent test client.
        let silent_fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM, 0) };
        assert!(silent_fd >= 0);
        let mut addr = std::mem::MaybeUninit::<libc::sockaddr_un>::uninit();
        // SAFETY: zero out sockaddr_un.
        unsafe {
            libc::memset(
                addr.as_mut_ptr() as *mut libc::c_void,
                0,
                std::mem::size_of::<libc::sockaddr_un>(),
            )
        };
        // SAFETY: assume_init after memset.
        let mut addr = unsafe { addr.assume_init() };
        addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
        for (dst, &src) in addr
            .sun_path
            .iter_mut()
            .zip(sock_path.as_os_str().as_bytes().iter())
        {
            *dst = src as libc::c_char;
        }

        // SAFETY: connect silent client.
        let conn_ret = unsafe {
            libc::connect(
                silent_fd,
                &addr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
            )
        };
        assert_eq!(conn_ret, 0);

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
        assert_eq!(server.active_clients(), 1);

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
            server.active_clients(),
            0,
            "silent client must be dropped after idle timeout"
        );

        // SAFETY: close silent client socket.
        unsafe { libc::close(silent_fd) };
        server.cleanup(&mut reactor);
    }
}
