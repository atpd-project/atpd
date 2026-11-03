pub mod credentials;

#[cfg(test)]
mod tests;

#[allow(unused_imports)]
pub use credentials::{
    service_credentials_resolve, service_credentials_resolve_with_lookups, ServiceCredentials,
};
