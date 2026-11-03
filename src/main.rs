#[allow(dead_code)]
mod config;
#[allow(dead_code)]
mod context;
#[allow(dead_code)]
mod logger;
#[allow(dead_code)]
mod nativeapi;
#[allow(dead_code)]
mod reactor;
#[allow(dead_code)]
mod supervisor;
#[allow(dead_code)]
mod utils;
#[allow(dead_code)]
mod version;

fn main() {
    println!("atpd {}", env!("CARGO_PKG_VERSION"));
}
