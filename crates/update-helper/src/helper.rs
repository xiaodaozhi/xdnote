// crates/xdnote-update-helper/src/helper.rs

//! Small native updater. It never accepts a command, trust key or destination from a release.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[cfg(target_os = "linux")]
#[allow(unsafe_code)]
#[path = "linux.rs"]
mod linux;
#[cfg(windows)]
#[allow(unsafe_code)]
#[path = "windows.rs"]
mod windows;
#[cfg(target_os = "linux")]
use linux as platform;
#[cfg(windows)]
use windows as platform;

type Result<T> = std::result::Result<T, &'static str>;
const TRUST: &str = include_str!("../../../tools/update-trust.json");
const CONTROL_LIMIT: u64 = 16 * 1024;
const PACKAGE_LIMIT: u64 = 2 * 1024 * 1024 * 1024;
const WAIT: Duration = Duration::from_secs(300);

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Request {
    schema_version: u32,
    transaction: String,
    parent_pid: u32,
    current_version: String,
    current_build: u32,
    install_root: PathBuf,
    package_file: PathBuf,
    manifest_file: PathBuf,
    signature_file: PathBuf,
}

#[derive(Debug, Deserialize)]
struct Identity {
    product: String,
    version: String,
    build: u32,
    revision: String,
    dirty: bool,
}

#[derive(Deserialize, Serialize)]
struct Control {
    transaction: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

struct Prepared {
    inspection: xdnote_update::Inspection,
    revision: String,
    package: File,
    #[cfg(target_os = "linux")]
    manifest: Vec<u8>,
    #[cfg(target_os = "linux")]
    signature: Vec<u8>,
}

fn hex_identity(value: &str, size: usize) -> bool {
    value.len() == size
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn read_bounded(mut file: File, limit: u64) -> Result<Vec<u8>> {
    let metadata = file.metadata().map_err(|_| "source_io")?;
    if !metadata.is_file() || metadata.len() > limit {
        return Err("source_limit");
    }
    let mut bytes = Vec::new();
    Read::by_ref(&mut file)
        .take(limit + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "source_io")?;
    if bytes.len() as u64 > limit {
        return Err("source_limit");
    }
    Ok(bytes)
}

fn child_name<'a>(job: &Path, path: &'a Path) -> Result<&'a std::ffi::OsStr> {
    if !path.is_absolute() || path.parent() != Some(job) {
        return Err("invalid_request_path");
    }
    path.file_name().ok_or("invalid_request_path")
}

fn validate_request(request: &Request) -> Result<()> {
    if request.schema_version != 1
        || !hex_identity(&request.transaction, 32)
        || request.parent_pid < 2
        || request.current_build == 0
        || request.current_version.len() > 64
        || !request.install_root.is_absolute()
    {
        return Err("invalid_request");
    }
    Ok(())
}

fn identity(executable: &Path) -> Result<Identity> {
    let mut child = Command::new(executable)
        .arg("--version-json")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|_| "identity_failed")?;
    let started = Instant::now();
    loop {
        if let Some(status) = child.try_wait().map_err(|_| "identity_failed")? {
            if !status.success() {
                return Err("identity_failed");
            }
            let output = child.stdout.take().ok_or("identity_failed")?;
            let mut bytes = Vec::new();
            output
                .take(CONTROL_LIMIT + 1)
                .read_to_end(&mut bytes)
                .map_err(|_| "identity_failed")?;
            if bytes.len() as u64 > CONTROL_LIMIT {
                return Err("identity_failed");
            }
            let value: Identity = serde_json::from_slice(&bytes).map_err(|_| "identity_failed")?;
            if value.product != "xdnote" || value.dirty || !hex_identity(&value.revision, 40) {
                return Err("identity_failed");
            }
            return Ok(value);
        }
        if started.elapsed() > Duration::from_secs(15) {
            // Only this diagnostic child is terminated, never the application or installer.
            let _ = child.kill();
            let _ = child.wait();
            return Err("identity_timeout");
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn prepare(job: &platform::Job, request: &Request) -> Result<Prepared> {
    let manifest = read_bounded(
        job.open(child_name(job.path(), &request.manifest_file)?)?,
        xdnote_update::MANIFEST_LIMIT as u64,
    )?;
    let signature = read_bounded(
        job.open(child_name(job.path(), &request.signature_file)?)?,
        CONTROL_LIMIT,
    )?;
    let envelope = std::str::from_utf8(&signature).map_err(|_| "invalid_signature")?;
    if !xdnote_update::verify_manifest(&manifest, envelope, TRUST).is_empty() {
        return Err("untrusted_signature");
    }
    let executable = platform::installed_executable(request)?;
    let current = identity(&executable)?;
    if current.version != request.current_version || current.build != request.current_build {
        return Err("installed_identity_changed");
    }
    let text = std::str::from_utf8(&manifest).map_err(|_| "invalid_manifest")?;
    let channel = if current.version.contains("-alpha.") {
        "alpha"
    } else if current.version.contains("-beta.") {
        "beta"
    } else if current.version.contains("-rc.") {
        "rc"
    } else {
        "stable"
    };
    let inspection = xdnote_update::inspect_release(
        text,
        &current.version,
        current.build,
        channel,
        platform::PLATFORM,
        "x86_64",
        platform::PACKAGE,
    );
    if !inspection.valid
        || !inspection.newer
        || inspection.bytes == 0
        || inspection.bytes > PACKAGE_LIMIT
    {
        return Err("invalid_update_target");
    }
    let name = child_name(job.path(), &request.package_file)?;
    if name != std::ffi::OsStr::new(&inspection.filename) {
        return Err("invalid_package_name");
    }
    let mut package = job.open(name)?;
    hash_package(&mut package, inspection.bytes, &inspection.sha256)?;
    let release: serde_json::Value =
        serde_json::from_slice(&manifest).map_err(|_| "invalid_manifest")?;
    let revision = release["commit"]
        .as_str()
        .ok_or("invalid_manifest")?
        .to_owned();
    Ok(Prepared {
        inspection,
        revision,
        package,
        #[cfg(target_os = "linux")]
        manifest,
        #[cfg(target_os = "linux")]
        signature,
    })
}

fn hash_package(file: &mut File, expected_size: u64, expected_hash: &str) -> Result<()> {
    use std::io::{Seek, SeekFrom};
    if file.metadata().map_err(|_| "package_io")?.len() != expected_size {
        return Err("package_size");
    }
    file.seek(SeekFrom::Start(0)).map_err(|_| "package_io")?;
    let mut hash = Sha256::new();
    let mut read = 0u64;
    let mut buffer = [0u8; 128 * 1024];
    loop {
        let length = file.read(&mut buffer).map_err(|_| "package_io")?;
        if length == 0 {
            break;
        }
        read = read.checked_add(length as u64).ok_or("package_size")?;
        if read > expected_size {
            return Err("package_size");
        }
        hash.update(&buffer[..length]);
    }
    if read != expected_size || format!("{:x}", hash.finalize()) != expected_hash {
        return Err("package_hash");
    }
    file.seek(SeekFrom::Start(0)).map_err(|_| "package_io")?;
    Ok(())
}

fn control(job: &platform::Job, name: &str, transaction: &str) -> Result<bool> {
    let Some(file) = job.control(name)? else {
        return Ok(false);
    };
    let bytes = read_bounded(file, CONTROL_LIMIT)?;
    let control: Control = serde_json::from_slice(&bytes).map_err(|_| "invalid_control")?;
    if control.transaction != transaction {
        return Err("transaction_mismatch");
    }
    Ok(true)
}

fn write_control(
    job: &platform::Job,
    name: &str,
    transaction: &str,
    error: Option<&str>,
) -> Result<()> {
    let bytes = serde_json::to_vec(&Control {
        transaction: transaction.into(),
        error: error.map(str::to_owned),
    })
    .map_err(|_| "control_io")?;
    job.write(name, &bytes)
}

fn execute(job: &platform::Job, request: &Request) -> Result<()> {
    let prepared = prepare(job, request)?;
    let parent = platform::Parent::open(job, request)?;
    let gate = platform::Gate::acquire(request)?;
    let staged = platform::stage(job, request, prepared)?;
    write_control(job, "ready.json", &request.transaction, None)?;
    let started = Instant::now();
    loop {
        if control(job, "cancel.json", &request.transaction)? {
            return Err("cancelled");
        }
        if control(job, "proceed.json", &request.transaction)? {
            break;
        }
        if parent.exited()? {
            return Err("parent_exited_early");
        }
        if started.elapsed() > WAIT {
            return Err("handoff_timeout");
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    while !parent.exited()? {
        if control(job, "cancel.json", &request.transaction)? {
            return Err("cancelled");
        }
        if started.elapsed() > WAIT {
            return Err("parent_exit_timeout");
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    platform::install(job, request, &staged)?;
    let installed = identity(&platform::updated_executable(request)?)?;
    if installed.version != staged.inspection.version || installed.build != staged.inspection.build
    {
        return Err("installed_identity_mismatch");
    }
    if installed.revision != staged.revision {
        return Err("installed_identity_mismatch");
    }
    write_control(job, "result.json", &request.transaction, Some(""))?;
    drop(gate);
    platform::restart(job, request, &parent)?;
    Ok(())
}

#[cfg(any(windows, target_os = "linux"))]
pub(super) fn run() -> Result<()> {
    let args: Vec<_> = std::env::args_os().collect();
    if args.len() != 3 {
        return Err("invalid_arguments");
    }
    let request_path = PathBuf::from(&args[2]);
    #[cfg(target_os = "linux")]
    if args[1] == "--authorize" {
        return platform::authorize(&request_path);
    }
    if args[1] != "--request" {
        return Err("invalid_arguments");
    }
    let job = platform::Job::load(&request_path)?;
    let bytes = read_bounded(
        job.open(std::ffi::OsStr::new("request.json"))?,
        CONTROL_LIMIT,
    )?;
    let request: Request = serde_json::from_slice(&bytes).map_err(|_| "invalid_request")?;
    validate_request(&request)?;
    let result = execute(&job, &request);
    if let Err(error) = result {
        let _ = write_control(&job, "result.json", &request.transaction, Some(error));
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn request_rejects_bad_transaction_pid_and_relative_paths() {
        let mut request: Request = serde_json::from_value(serde_json::json!({
            "schema_version":1,"transaction":"a".repeat(32),"parent_pid":42,
            "current_version":"0.1.0-alpha.6","current_build":19,
            "install_root":std::env::temp_dir(),"package_file":"x","manifest_file":"x","signature_file":"x"
        })).unwrap();
        assert!(validate_request(&request).is_ok());
        request.transaction = "A".repeat(32);
        assert_eq!(validate_request(&request), Err("invalid_request"));
        request.transaction = "a".repeat(32);
        request.parent_pid = 1;
        assert_eq!(validate_request(&request), Err("invalid_request"));
    }
    #[test]
    fn controls_never_reference_outside_job_directory() {
        let root = std::env::temp_dir().join("xdnote-test-job");
        assert!(child_name(&root, &root.join("release.json")).is_ok());
        assert!(child_name(&root, &root.join("../release.json")).is_err());
        assert!(child_name(&root, Path::new("release.json")).is_err());
    }
    #[test]
    fn empty_production_trust_never_accepts_an_envelope() {
        assert!(!xdnote_update::verify_manifest(b"{}", "{}", TRUST).is_empty());
    }
    #[test]
    fn package_meter_rejects_wrong_size_hash_and_nonregular_objects() {
        let root = std::env::temp_dir().join(format!(
            "xdnote-helper-unit-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&root).unwrap();
        let path = root.join("package.bin");
        fs::write(&path, b"owned updater fixture").unwrap();
        let expected = format!("{:x}", Sha256::digest(b"owned updater fixture"));
        let mut file = File::open(&path).unwrap();
        assert!(hash_package(&mut file, 21, &expected).is_ok());
        assert_eq!(hash_package(&mut file, 20, &expected), Err("package_size"));
        assert_eq!(
            hash_package(&mut file, 21, &"0".repeat(64)),
            Err("package_hash")
        );
        assert!(read_bounded(File::open(&path).unwrap(), 4).is_err());
        fs::remove_file(path).unwrap();
        fs::remove_dir(root).unwrap();
    }
    #[test]
    fn control_transaction_is_required_and_never_silently_rebound() {
        assert!(serde_json::from_str::<Control>("{}").is_err());
        let value: Control =
            serde_json::from_str(r#"{"transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}"#).unwrap();
        assert_eq!(value.transaction, "a".repeat(32));
        assert!(value.error.is_none());
    }
}
