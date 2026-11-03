//! Unix Domain Socket runtime command interface.
//!
//! Mirrors `src/uds.c` and `include/uds.h`.

pub mod server;

pub use server::{query_daemon, IpcRequest, IpcServer};
