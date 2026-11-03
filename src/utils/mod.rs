pub mod proc;

#[cfg(test)]
mod tests;

#[allow(unused_imports)]
pub use proc::{parse_stat_starttime, process_starttime};
