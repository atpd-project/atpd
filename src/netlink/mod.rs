//! Netlink and XFRM monitoring and VPN bypass rule management.

pub mod bypass;
pub mod msg;
pub mod stats;
pub mod watcher;

#[cfg(test)]
mod tests;

pub use watcher::{is_proxy_interface, NetlinkWatcher};
