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

fn main() {
    println!("atpd {}", env!("CARGO_PKG_VERSION"));
}
