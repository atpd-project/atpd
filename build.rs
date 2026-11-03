use std::fs;
use std::path::Path;
use std::process::Command;

fn main() {
    let mut product_version = String::new();

    // 1. Check VERSION environment variable
    if let Ok(v) = std::env::var("VERSION") {
        let trimmed = v.trim();
        if !trimmed.is_empty() {
            product_version = trimmed.to_string();
        }
    }

    // 2. Check VERSION file in crate directory
    if product_version.is_empty() {
        if let Ok(content) = fs::read_to_string("VERSION") {
            let trimmed = content.trim();
            if !trimmed.is_empty() {
                product_version = trimmed.to_string();
            }
        }
    }

    // 3. Fallback to CARGO_PKG_VERSION
    if product_version.is_empty() {
        product_version = std::env::var("CARGO_PKG_VERSION")
            .unwrap_or_else(|_| "0.1.0".to_string())
            .trim()
            .to_string();
    }

    // Git metadata inspection
    let mut commit = "unknown".to_string();
    let mut release_tag = String::new();
    let mut dirty = false;

    if let Ok(output) = Command::new("git")
        .args(["rev-parse", "--short=12", "HEAD"])
        .output()
    {
        if output.status.success() {
            let s = String::from_utf8_lossy(&output.stdout).trim().to_string();
            if !s.is_empty() {
                commit = s;
            }
        }
    }

    if commit != "unknown" {
        if let Ok(output) = Command::new("git")
            .args(["describe", "--tags", "--exact-match", "HEAD"])
            .output()
        {
            if output.status.success() {
                release_tag = String::from_utf8_lossy(&output.stdout).trim().to_string();
            }
        }

        if let Ok(status) = Command::new("git")
            .args(["diff-index", "--quiet", "HEAD", "--"])
            .status()
        {
            if !status.success() {
                dirty = true;
            }
        }
    }

    // Generate FULL_VERSION matching gen_version.sh
    let full_version = if commit == "unknown" {
        format!("{}-dev+unknown", product_version)
    } else if !dirty && release_tag == format!("v{}", product_version) {
        product_version.clone()
    } else {
        let mut fv = format!("{}-dev+g{}", product_version, commit);
        if dirty {
            fv.push_str(".dirty");
        }
        fv
    };

    println!("cargo:rustc-env=ATPD_VERSION={}", product_version);
    println!("cargo:rustc-env=ATPD_FULL_VERSION={}", full_version);
    println!("cargo:rustc-env=ATPD_COMMIT={}", commit);

    println!("cargo:rerun-if-env-changed=VERSION");
    println!("cargo:rerun-if-changed=build.rs");
    if Path::new(".git/HEAD").exists() {
        println!("cargo:rerun-if-changed=.git/HEAD");
    }
    if Path::new(".git/index").exists() {
        println!("cargo:rerun-if-changed=.git/index");
    }
}
