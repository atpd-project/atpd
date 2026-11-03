//! Blocking HTTP/1.1 gRPC-Web client for sing-box Native API.
//!
//! Mirrors `src/singbox_api.c` over standard TCP sockets with bounded connect
//! and I/O timeouts, matching sing-box dashboard semantics.

use super::proto::{self, ProtoError};
use super::status::{SingboxClashModeStatus, SingboxStatus};
use crate::config::Config;
use std::fmt;
use std::io::{Read, Write};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, TcpStream};
use std::time::Duration;

pub const DEFAULT_API_HOST: &str = "127.0.0.1";
pub const DEFAULT_API_PORT: u16 = 9080;
pub const DEFAULT_API_TIMEOUT_SEC: u64 = 2;

const SUBSCRIBE_STATUS_REQUEST_FRAME: [u8; 11] = [
    0x00, 0x00, 0x00, 0x00, 0x06, 0x08, 0x80, 0x94, 0xeb, 0xdc, 0x03,
];

#[derive(Debug)]
pub enum ClientError {
    InvalidHost(String),
    Io(std::io::Error),
    Proto(ProtoError),
    IncompleteResponse,
    InvalidMode(String),
}

impl fmt::Display for ClientError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ClientError::InvalidHost(h) => write!(f, "invalid IPv4 host: {}", h),
            ClientError::Io(e) => write!(f, "I/O error: {}", e),
            ClientError::Proto(p) => write!(f, "protocol error: {}", p),
            ClientError::IncompleteResponse => write!(f, "incomplete HTTP response from server"),
            ClientError::InvalidMode(m) => write!(f, "invalid clash mode: {}", m),
        }
    }
}

impl std::error::Error for ClientError {}

impl From<std::io::Error> for ClientError {
    fn from(err: std::io::Error) -> Self {
        ClientError::Io(err)
    }
}

impl From<ProtoError> for ClientError {
    fn from(err: ProtoError) -> Self {
        ClientError::Proto(err)
    }
}

#[derive(Debug, Clone)]
pub struct Client {
    host: String,
    port: u16,
    secret: String,
    timeout: Duration,
}

impl Client {
    pub fn new(
        host: &str,
        port: u16,
        secret: &str,
        timeout: Duration,
    ) -> Result<Self, ClientError> {
        let clean_host = if host.is_empty() {
            DEFAULT_API_HOST
        } else {
            host
        };

        // Strictly validate IPv4 address matching C inet_pton(AF_INET, ...) check.
        clean_host
            .parse::<Ipv4Addr>()
            .map_err(|_| ClientError::InvalidHost(clean_host.to_string()))?;

        let effective_port = if port > 0 { port } else { DEFAULT_API_PORT };

        Ok(Self {
            host: clean_host.to_string(),
            port: effective_port,
            secret: secret.to_string(),
            timeout,
        })
    }

    pub fn from_config(cfg: &Config) -> Result<Self, ClientError> {
        Self::new(
            &cfg.api.host,
            if cfg.api.port > 0 {
                cfg.api.port as u16
            } else {
                DEFAULT_API_PORT
            },
            &cfg.api.secret,
            Duration::from_secs(DEFAULT_API_TIMEOUT_SEC),
        )
    }

    pub fn host(&self) -> &str {
        &self.host
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn secret(&self) -> &str {
        &self.secret
    }

    pub fn timeout(&self) -> Duration {
        self.timeout
    }

    fn socket_addr(&self) -> Result<SocketAddr, ClientError> {
        let ip = self
            .host
            .parse::<Ipv4Addr>()
            .map_err(|_| ClientError::InvalidHost(self.host.clone()))?;
        Ok(SocketAddr::V4(SocketAddrV4::new(ip, self.port)))
    }

    fn connect(&self) -> Result<TcpStream, ClientError> {
        let addr = self.socket_addr()?;
        let stream = TcpStream::connect_timeout(&addr, self.timeout)?;
        stream.set_read_timeout(Some(self.timeout))?;
        stream.set_write_timeout(Some(self.timeout))?;
        Ok(stream)
    }

    /// Health probe connecting to sing-box API port with bounded 1s timeout.
    /// Mirrors `singbox_api_health_check` in singbox_api.c.
    pub fn health_check(&self) -> Result<(), ClientError> {
        let _stream = self.connect()?;
        Ok(())
    }

    /// Retrieve telemetry sample via SubscribeStatus RPC.
    /// Mirrors `singbox_api_get_status` in singbox_api.c.
    pub fn get_status(&self) -> Result<SingboxStatus, ClientError> {
        let mut stream = self.connect()?;

        let mut req = Vec::with_capacity(512);
        req.extend_from_slice(
            format!(
                "POST /daemon.StartedService/SubscribeStatus HTTP/1.1\r\nHost: {}:{}\r\nContent-Type: application/grpc-web+proto\r\nX-Grpc-Web: 1\r\nAccept: application/grpc-web+proto\r\nContent-Length: 11\r\nUser-Agent: ATPd-Native/2.0\r\n",
                self.host, self.port
            )
            .as_bytes(),
        );

        if !self.secret.is_empty() {
            req.extend_from_slice(format!("Authorization: Bearer {}\r\n", self.secret).as_bytes());
        }
        req.extend_from_slice(b"Connection: close\r\n\r\n");
        req.extend_from_slice(&SUBSCRIBE_STATUS_REQUEST_FRAME);

        stream.write_all(&req)?;

        let mut buf = [0u8; 4096];
        let mut used = 0;

        while used < buf.len() {
            let n = match stream.read(&mut buf[used..]) {
                Ok(0) => break,
                Ok(n) => n,
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(ClientError::Io(e)),
            };
            used += n;

            if let Some(status) = proto::parse_grpc_web_response(&buf[..used])? {
                return Ok(status);
            }
        }

        if let Some(status) = proto::parse_grpc_web_response(&buf[..used])? {
            Ok(status)
        } else {
            Err(ClientError::IncompleteResponse)
        }
    }

    /// Perform a unary gRPC-Web RPC over HTTP/1.1.
    /// Mirrors `grpc_web_unary_call` in singbox_api.c.
    pub fn unary_call(&self, path: &str, request_payload: &[u8]) -> Result<Vec<u8>, ClientError> {
        if request_payload.len() > 256 {
            return Err(ClientError::Proto(ProtoError::ValueOutOfRange));
        }

        let mut stream = self.connect()?;
        let total_body_len = request_payload.len() + 5;

        let mut req = Vec::with_capacity(512 + total_body_len);
        req.extend_from_slice(
            format!(
                "POST {} HTTP/1.1\r\nHost: {}:{}\r\nContent-Type: application/grpc-web+proto\r\nX-Grpc-Web: 1\r\nAccept: application/grpc-web+proto\r\nContent-Length: {}\r\nUser-Agent: ATPd-Native/2.0\r\n",
                path, self.host, self.port, total_body_len
            )
            .as_bytes(),
        );

        if !self.secret.is_empty() {
            req.extend_from_slice(format!("Authorization: Bearer {}\r\n", self.secret).as_bytes());
        }
        req.extend_from_slice(b"Connection: close\r\n\r\n");

        // 5-byte gRPC-Web frame header: [flags (0x00), 4-byte big endian message length]
        req.push(0x00);
        req.extend_from_slice(&(request_payload.len() as u32).to_be_bytes());
        req.extend_from_slice(request_payload);

        stream.write_all(&req)?;

        let mut buf = [0u8; 8192];
        let mut used = 0;

        while used < buf.len() {
            let n = match stream.read(&mut buf[used..]) {
                Ok(0) => break,
                Ok(n) => n,
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(ClientError::Io(e)),
            };
            used += n;

            if let Some(frame) = proto::extract_unary_frame(&buf[..used])? {
                return Ok(frame.to_vec());
            }
        }

        if let Some(frame) = proto::extract_unary_frame(&buf[..used])? {
            Ok(frame.to_vec())
        } else {
            Err(ClientError::IncompleteResponse)
        }
    }

    /// Query version via GetVersion RPC.
    /// Mirrors `singbox_api_get_version` in singbox_api.c.
    pub fn get_version(&self) -> Result<String, ClientError> {
        let frame = self.unary_call("/daemon.StartedService/GetVersion", &[])?;
        let ver = proto::parse_string_field(&frame, 1)?;
        Ok(ver)
    }

    /// Query Clash mode status via GetClashModeStatus RPC.
    /// Mirrors `singbox_api_get_clash_mode_status` in singbox_api.c.
    pub fn get_clash_mode_status(&self) -> Result<SingboxClashModeStatus, ClientError> {
        let frame = self.unary_call("/daemon.StartedService/GetClashModeStatus", &[])?;
        let status = proto::parse_clash_mode_status(&frame)?;
        Ok(status)
    }

    /// Query current Clash mode.
    /// Mirrors `singbox_api_get_clash_mode` in singbox_api.c.
    pub fn get_clash_mode(&self) -> Result<String, ClientError> {
        let status = self.get_clash_mode_status()?;
        Ok(status.current_mode)
    }

    /// Set current Clash mode via SetClashMode RPC.
    /// Encodes field 3 (`string mode = 3`, tag 0x1A), matching C client and proto.
    /// Mirrors `singbox_api_set_clash_mode` in singbox_api.c.
    pub fn set_clash_mode(&self, mode: &str) -> Result<(), ClientError> {
        if mode.is_empty() {
            return Err(ClientError::InvalidMode("empty mode".to_string()));
        }
        let encoded_body = proto::encode_set_clash_mode(mode)?;
        let _ = self.unary_call("/daemon.StartedService/SetClashMode", &encoded_body)?;
        Ok(())
    }
}
