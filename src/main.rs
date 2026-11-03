#[allow(dead_code)]
mod config;
#[allow(dead_code)]
mod reactor;
#[allow(dead_code)]
mod supervisor;
#[allow(dead_code)]
mod utils;

fn main() {
    println!("atpd {}", env!("CARGO_PKG_VERSION"));
}
