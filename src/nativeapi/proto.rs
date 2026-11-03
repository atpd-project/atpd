//! Hand-written Protobuf varint / length-delimited codecs and gRPC-Web frame parser.
//!
//! Mirrors singbox_api.c decode logic and daemon/started_service.proto definitions without
//! third-party dependencies.

use super::status::{SingboxClashModeStatus, SingboxStatus, SINGBOX_MAX_CLASH_MODES};
use std::fmt;

#[derive(Debug, PartialEq, Eq, Clone)]
pub enum ProtoError {
    UnexpectedEof,
    InvalidVarint,
    InvalidWireType(u32),
    TrailersFrame,
    BufferOverflow,
    InvalidString,
    ValueOutOfRange,
    MalformedChunk,
    FieldNotFound(u32),
}

impl fmt::Display for ProtoError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ProtoError::UnexpectedEof => write!(f, "unexpected EOF while reading protobuf"),
            ProtoError::InvalidVarint => write!(f, "invalid varint encoding"),
            ProtoError::InvalidWireType(w) => write!(f, "unsupported wire type {}", w),
            ProtoError::TrailersFrame => write!(f, "gRPC-Web trailers frame or error response"),
            ProtoError::BufferOverflow => write!(f, "field length exceeds frame bounds"),
            ProtoError::InvalidString => write!(f, "invalid UTF-8 in string field"),
            ProtoError::ValueOutOfRange => write!(f, "value exceeds permitted range"),
            ProtoError::MalformedChunk => write!(f, "malformed HTTP chunk framing"),
            ProtoError::FieldNotFound(id) => write!(f, "protobuf field {} not found", id),
        }
    }
}

impl std::error::Error for ProtoError {}

/// Decode a protobuf varint from `buf` starting at `*pos`.
/// Advances `*pos` on success. Bounds-checked up to 64 bits.
pub fn read_varint(buf: &[u8], pos: &mut usize) -> Result<u64, ProtoError> {
    let mut result: u64 = 0;
    let mut shift: u32 = 0;
    while *pos < buf.len() && shift < 64 {
        let byte = buf[*pos];
        *pos += 1;
        result |= ((byte & 0x7f) as u64) << shift;
        if (byte & 0x80) == 0 {
            return Ok(result);
        }
        shift += 7;
    }
    if shift >= 64 {
        Err(ProtoError::InvalidVarint)
    } else {
        Err(ProtoError::UnexpectedEof)
    }
}

/// Encode a 64-bit integer into protobuf varint bytes.
pub fn encode_varint(mut value: u64, buf: &mut Vec<u8>) {
    while value >= 0x80 {
        buf.push(((value & 0x7f) as u8) | 0x80);
        value >>= 7;
    }
    buf.push((value & 0x7f) as u8);
}

/// Encode `ClashMode` message for SetClashMode RPC.
/// Encodes field 3 (`string mode = 3;`, tag byte `0x1A = (3 << 3) | 2`),
/// matching the C client and daemon/started_service.proto.
pub fn encode_set_clash_mode(mode: &str) -> Result<Vec<u8>, ProtoError> {
    if mode.is_empty() || mode.len() > 255 {
        return Err(ProtoError::ValueOutOfRange);
    }
    let mode_bytes = mode.as_bytes();
    let mut out = Vec::with_capacity(2 + mode_bytes.len());
    // Protobuf field 3: Tag 0x1A = (3 << 3) | 2
    out.push(0x1a);
    encode_varint(mode_bytes.len() as u64, &mut out);
    out.extend_from_slice(mode_bytes);
    Ok(out)
}

/// Decode the first gRPC-Web data frame of `SubscribeStatus`.
/// Mirrors `parse_status_frame` in singbox_api.c:
/// Returns:
/// - `Ok(Some(status))` on complete, valid status frame
/// - `Ok(None)` if frame is incomplete (needs more bytes from TCP stream)
/// - `Err(ProtoError)` on corrupted inputs, trailers frame, or int overflow
pub fn parse_status_frame(buf: &[u8]) -> Result<Option<SingboxStatus>, ProtoError> {
    if buf.len() < 5 {
        return Ok(None);
    }
    if (buf[0] & 0x80) != 0 {
        return Err(ProtoError::TrailersFrame);
    }

    let message_len = u32::from_be_bytes([buf[1], buf[2], buf[3], buf[4]]) as usize;
    if message_len > buf.len() - 5 {
        return Ok(None);
    }

    let mut status = SingboxStatus::default();
    let mut pos = 5;
    let end = 5 + message_len;
    // Varints must not read past this message into trailing bytes.
    let buf = &buf[..end];

    while pos < end {
        let key = read_varint(buf, &mut pos)?;
        let field = (key >> 3) as u32;
        let wire = (key & 7) as u32;

        match wire {
            0 => {
                let value = read_varint(buf, &mut pos)?;
                match field {
                    1 => status.memory = value,
                    2 => {
                        if value > i32::MAX as u64 {
                            return Err(ProtoError::ValueOutOfRange);
                        }
                        status.goroutines = value as i32;
                    }
                    3 => {
                        if value > i32::MAX as u64 {
                            return Err(ProtoError::ValueOutOfRange);
                        }
                        status.connections_in = value as i32;
                    }
                    4 => {
                        if value > i32::MAX as u64 {
                            return Err(ProtoError::ValueOutOfRange);
                        }
                        status.connections_out = value as i32;
                    }
                    5 => status.traffic_available = value != 0,
                    6 => status.uplink = value as i64,
                    7 => status.downlink = value as i64,
                    8 => status.uplink_total = value as i64,
                    9 => status.downlink_total = value as i64,
                    _ => {}
                }
            }
            1 => {
                if end - pos < 8 {
                    return Err(ProtoError::BufferOverflow);
                }
                pos += 8;
            }
            2 => {
                let size = read_varint(buf, &mut pos)?;
                if size > (end - pos) as u64 {
                    return Err(ProtoError::BufferOverflow);
                }
                pos += size as usize;
            }
            5 => {
                if end - pos < 4 {
                    return Err(ProtoError::BufferOverflow);
                }
                pos += 4;
            }
            _ => return Err(ProtoError::InvalidWireType(wire)),
        }
    }

    Ok(Some(status))
}

/// Parse HTTP/1.1 response framing used by sing-box's gRPC-Web bridge.
/// Handles both raw (Content-Length) and chunked transfer encoding.
/// Mirrors `parse_grpc_web_response` in singbox_api.c.
pub fn parse_grpc_web_response(buf: &[u8]) -> Result<Option<SingboxStatus>, ProtoError> {
    let header_end = match find_subsequence(buf, b"\r\n\r\n") {
        Some(pos) => pos,
        None => return Ok(None),
    };

    let mut body = header_end + 4;
    let headers = &buf[..header_end];
    let chunked =
        find_subsequence_case_insensitive(headers, b"transfer-encoding: chunked").is_some();

    if !chunked {
        if body <= buf.len() {
            return parse_status_frame(&buf[body..]);
        }
        return Ok(None);
    }

    while body < buf.len() {
        let line_end = match find_subsequence(&buf[body..], b"\r\n") {
            Some(pos) => body + pos,
            None => return Ok(None),
        };

        let line = &buf[body..line_end];
        if line.is_empty() || line.len() >= 32 {
            return Err(ProtoError::MalformedChunk);
        }

        let size_str = match std::str::from_utf8(line) {
            Ok(s) => s.trim(),
            Err(_) => return Err(ProtoError::MalformedChunk),
        };

        let chunk_len = match usize::from_str_radix(size_str, 16) {
            Ok(len) => len,
            Err(_) => return Err(ProtoError::MalformedChunk),
        };

        body = line_end + 2;
        if chunk_len == 0 {
            return Err(ProtoError::TrailersFrame);
        }
        if chunk_len > buf.len() - body {
            return Ok(None);
        }

        let chunk = &buf[body..body + chunk_len];
        if let Some(status) = parse_status_frame(chunk)? {
            return Ok(Some(status));
        }
        body += chunk_len + 2;
    }

    Ok(None)
}

/// Extract unary response gRPC-Web frame (5-byte header + protobuf payload) from an HTTP response.
pub fn extract_unary_frame(buf: &[u8]) -> Result<Option<&[u8]>, ProtoError> {
    let header_end = match find_subsequence(buf, b"\r\n\r\n") {
        Some(pos) => pos,
        None => return Ok(None),
    };

    let body = header_end + 4;
    let headers = &buf[..header_end];
    let chunked =
        find_subsequence_case_insensitive(headers, b"transfer-encoding: chunked").is_some();

    let (frame_offset, frame_size) = if chunked {
        if body >= buf.len() {
            return Ok(None);
        }
        let line_end = match find_subsequence(&buf[body..], b"\r\n") {
            Some(pos) => body + pos,
            None => return Ok(None),
        };
        let line = &buf[body..line_end];
        if line.is_empty() || line.len() >= 32 {
            return Err(ProtoError::MalformedChunk);
        }
        let size_str = match std::str::from_utf8(line) {
            Ok(s) => s.trim(),
            Err(_) => return Err(ProtoError::MalformedChunk),
        };
        let chunk_len = match usize::from_str_radix(size_str, 16) {
            Ok(len) => len,
            Err(_) => return Err(ProtoError::MalformedChunk),
        };
        let offset = line_end + 2;
        if chunk_len == 0 || chunk_len > buf.len() - offset {
            return Ok(None);
        }
        (offset, chunk_len)
    } else {
        (body, buf.len() - body)
    };

    if frame_size < 5 {
        return Ok(None);
    }
    if (buf[frame_offset] & 0x80) != 0 {
        return Err(ProtoError::TrailersFrame);
    }

    let message_len = u32::from_be_bytes([
        buf[frame_offset + 1],
        buf[frame_offset + 2],
        buf[frame_offset + 3],
        buf[frame_offset + 4],
    ]) as usize;

    if message_len > frame_size - 5 {
        return Ok(None);
    }

    Ok(Some(&buf[frame_offset..frame_offset + 5 + message_len]))
}

/// Parse a string field by tag number from a gRPC-Web frame.
/// Mirrors `parse_string_field` in singbox_api.c.
pub fn parse_string_field(frame: &[u8], wanted_field: u32) -> Result<String, ProtoError> {
    if frame.len() < 5 || (frame[0] & 0x80) != 0 {
        return Err(ProtoError::TrailersFrame);
    }

    let message_len = u32::from_be_bytes([frame[1], frame[2], frame[3], frame[4]]) as usize;
    if message_len > frame.len() - 5 {
        return Err(ProtoError::BufferOverflow);
    }

    let mut pos = 5;
    let end = 5 + message_len;
    // Varints must not read past this message into trailing bytes.
    let frame = &frame[..end];

    while pos < end {
        let key = read_varint(frame, &mut pos)?;
        let field = (key >> 3) as u32;
        let wire = (key & 7) as u32;

        match wire {
            0 => {
                let _ = read_varint(frame, &mut pos)?;
            }
            1 => {
                if end - pos < 8 {
                    return Err(ProtoError::BufferOverflow);
                }
                pos += 8;
            }
            2 => {
                let size = read_varint(frame, &mut pos)?;
                if size > (end - pos) as u64 {
                    return Err(ProtoError::BufferOverflow);
                }
                let str_bytes = &frame[pos..pos + size as usize];
                pos += size as usize;
                if field == wanted_field {
                    return std::str::from_utf8(str_bytes)
                        .map(|s| s.to_string())
                        .map_err(|_| ProtoError::InvalidString);
                }
            }
            5 => {
                if end - pos < 4 {
                    return Err(ProtoError::BufferOverflow);
                }
                pos += 4;
            }
            _ => return Err(ProtoError::InvalidWireType(wire)),
        }
    }

    Err(ProtoError::FieldNotFound(wanted_field))
}

/// Parse `ClashModeStatus` from a gRPC-Web frame.
/// Mirrors `parse_clash_mode_status` in singbox_api.c.
pub fn parse_clash_mode_status(frame: &[u8]) -> Result<SingboxClashModeStatus, ProtoError> {
    if frame.len() < 5 || (frame[0] & 0x80) != 0 {
        return Err(ProtoError::TrailersFrame);
    }

    let message_len = u32::from_be_bytes([frame[1], frame[2], frame[3], frame[4]]) as usize;
    if message_len > frame.len() - 5 {
        return Err(ProtoError::BufferOverflow);
    }

    let mut status = SingboxClashModeStatus::default();
    let mut pos = 5;
    let end = 5 + message_len;
    // Varints must not read past this message into trailing bytes.
    let frame = &frame[..end];

    while pos < end {
        let key = read_varint(frame, &mut pos)?;
        let field = (key >> 3) as u32;
        let wire = (key & 7) as u32;

        match wire {
            0 => {
                let _ = read_varint(frame, &mut pos)?;
            }
            1 => {
                if end - pos < 8 {
                    return Err(ProtoError::BufferOverflow);
                }
                pos += 8;
            }
            2 => {
                let size = read_varint(frame, &mut pos)?;
                if size > (end - pos) as u64 {
                    return Err(ProtoError::BufferOverflow);
                }
                let str_bytes = &frame[pos..pos + size as usize];
                pos += size as usize;
                let text = match std::str::from_utf8(str_bytes) {
                    Ok(t) => t.to_string(),
                    Err(_) => return Err(ProtoError::InvalidString),
                };
                if field == 1 && status.modes.len() < SINGBOX_MAX_CLASH_MODES {
                    status.modes.push(text);
                } else if field == 2 {
                    status.current_mode = text;
                }
            }
            5 => {
                if end - pos < 4 {
                    return Err(ProtoError::BufferOverflow);
                }
                pos += 4;
            }
            _ => return Err(ProtoError::InvalidWireType(wire)),
        }
    }

    if status.current_mode.is_empty() {
        return Err(ProtoError::FieldNotFound(2));
    }

    Ok(status)
}

fn find_subsequence(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|w| w == needle)
}

fn find_subsequence_case_insensitive(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack
        .windows(needle.len())
        .position(|w| w.eq_ignore_ascii_case(needle))
}

#[cfg(test)]
mod fuzz_tests {
    use super::*;

    // Parsers see bytes from the network; with panic = "abort" any panic would kill the daemon.
    #[test]
    fn parsers_never_panic_on_random_or_mutated_input() {
        let mut seed: u64 = 0x9e37_79b9_7f4a_7c15;
        let mut next = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let seeds: [&[u8]; 3] = [
            b"\x00\x00\x00\x00\x08\x0a\x04Rule\x12\x02ok",
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n9\r\n\x00\x00\x00\x00\x04\x08\x01\x10\x02\r\n0\r\n\r\n",
            b"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n\x00\x00\x00\x00\x04\x0a\x02ab",
        ];
        for i in 0..200_000 {
            let mut buf: Vec<u8> = if i % 2 == 0 {
                (0..(next() % 64) as usize).map(|_| next() as u8).collect()
            } else {
                seeds[i % seeds.len()].to_vec()
            };
            if !buf.is_empty() {
                for _ in 0..(next() % 4) {
                    let idx = (next() as usize) % buf.len();
                    buf[idx] = next() as u8;
                }
                buf.truncate((next() as usize) % (buf.len() + 1));
            }
            let _ = parse_status_frame(&buf);
            let _ = parse_grpc_web_response(&buf);
            let _ = extract_unary_frame(&buf);
            let _ = parse_string_field(&buf, (next() % 5) as u32);
            let _ = parse_clash_mode_status(&buf);
        }
    }
}
