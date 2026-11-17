// crates/xdnote-update-helper/src/main.rs

//! Standalone signed-update installation runner, deliberately independent of Qt.

#![cfg_attr(all(windows, not(test)), windows_subsystem = "windows")]

#[cfg(any(windows, target_os = "linux"))]
mod helper;

fn main() {
    #[cfg(any(windows, target_os = "linux"))]
    if let Err(error) = helper::run() {
        eprintln!("xdnote-update-helper: {error}");
        std::process::exit(1);
    }
    #[cfg(not(any(windows, target_os = "linux")))]
    std::process::exit(2);
}
