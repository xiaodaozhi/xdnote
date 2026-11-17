// crates/xdnote-update-helper/src/linux.rs

//! Root operations are fixed to one verified xdnote Debian package and owned runtime paths.

use super::*;
use std::collections::BTreeMap;
use std::ffi::{CStr, CString, OsStr};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};

pub(super) const PLATFORM: &str = "linux";
pub(super) const PACKAGE: &str = "deb";

fn cstring(value: &OsStr) -> Result<CString> {
    CString::new(value.as_bytes()).map_err(|_| "invalid_request_path")
}
fn directory(path: &Path) -> Result<File> {
    if !path.is_absolute() {
        return Err("invalid_request_path");
    }
    let root = CString::new("/").map_err(|_| "source_io")?;
    // SAFETY: constant path and flags return a new descriptor, immediately owned by File.
    let fd = unsafe {
        libc::open(
            root.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
        )
    };
    if fd < 0 {
        return Err("source_io");
    }
    let mut result = unsafe { File::from_raw_fd(fd) };
    for part in path.components() {
        match part {
            std::path::Component::RootDir => continue,
            std::path::Component::Normal(name) => {
                let name = cstring(name)?;
                // SAFETY: each relative open is anchored to the previous held directory;
                // O_NOFOLLOW rejects links instead of following a replaced path component.
                let fd = unsafe {
                    libc::openat(
                        result.as_raw_fd(),
                        name.as_ptr(),
                        libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                    )
                };
                if fd < 0 {
                    return Err("unsafe_job_directory");
                }
                result = unsafe { File::from_raw_fd(fd) };
            }
            _ => return Err("invalid_request_path"),
        }
    }
    Ok(result)
}

fn open_at(dir: &File, name: &OsStr, flags: i32, mode: u32) -> Result<File> {
    if Path::new(name).file_name() != Some(name) {
        return Err("invalid_request_path");
    }
    let name = cstring(name)?;
    // SAFETY: the NUL-free leaf is relative to the live directory descriptor;
    // nonblocking/no-follow prevents FIFO blocking and link traversal before classification.
    let fd = unsafe {
        libc::openat(
            dir.as_raw_fd(),
            name.as_ptr(),
            flags | libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK,
            mode,
        )
    };
    if fd < 0 {
        return Err("source_io");
    }
    Ok(unsafe { File::from_raw_fd(fd) })
}

struct Account {
    uid: u32,
    gid: u32,
    home: String,
}
fn account(uid: u32) -> Result<Account> {
    let mut record = std::mem::MaybeUninit::<libc::passwd>::uninit();
    let mut result = std::ptr::null_mut();
    let mut buffer = vec![0u8; 65536];
    // SAFETY: passwd output, result pointer and backing string buffer remain live until copied.
    let status = unsafe {
        libc::getpwuid_r(
            uid,
            record.as_mut_ptr(),
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            &mut result,
        )
    };
    if status != 0 || result.is_null() {
        return Err("invalid_user");
    }
    let record = unsafe { record.assume_init() };
    if record.pw_uid != uid || record.pw_dir.is_null() {
        return Err("invalid_user");
    }
    let home = unsafe { CStr::from_ptr(record.pw_dir) }
        .to_str()
        .map_err(|_| "invalid_user")?
        .to_owned();
    Ok(Account {
        uid,
        gid: record.pw_gid,
        home,
    })
}

pub(super) struct Job {
    path: PathBuf,
    dir: File,
    user: Account,
}
impl Job {
    pub(super) fn load(request: &Path) -> Result<Self> {
        // Only pkexec's fixed root helper path may perform this mode; root has no GUI credentials.
        if unsafe { libc::geteuid() } != 0 {
            return Err("authorization_required");
        }
        let uid = std::env::var("PKEXEC_UID")
            .ok()
            .and_then(|value| value.parse::<u32>().ok())
            .filter(|value| *value > 0)
            .ok_or("invalid_user")?;
        Self::for_user(request, uid)
    }
    fn for_user(request: &Path, uid: u32) -> Result<Self> {
        if !request.is_absolute() || request.file_name() != Some(OsStr::new("request.json")) {
            return Err("invalid_request_path");
        }
        let path = request
            .parent()
            .ok_or("invalid_request_path")?
            .to_path_buf();
        let dir = directory(&path)?;
        let metadata = dir.metadata().map_err(|_| "unsafe_job_directory")?;
        if metadata.uid() != uid || metadata.mode() & 0o077 != 0 {
            return Err("unsafe_job_directory");
        }
        Ok(Self {
            path,
            dir,
            user: account(uid)?,
        })
    }
    pub(super) fn path(&self) -> &Path {
        &self.path
    }
    pub(super) fn open(&self, name: &OsStr) -> Result<File> {
        let file = open_at(&self.dir, name, libc::O_RDONLY, 0)?;
        let metadata = file.metadata().map_err(|_| "source_io")?;
        if !metadata.is_file()
            || metadata.uid() != self.user.uid
            || metadata.nlink() != 1
            || metadata.mode() & 0o022 != 0
        {
            return Err("unsafe_source");
        }
        Ok(file)
    }
    pub(super) fn control(&self, name: &str) -> Result<Option<File>> {
        let name = cstring(OsStr::new(name))?;
        // SAFETY: existence is checked without following the leaf; open() rechecks classification.
        let mut metadata = std::mem::MaybeUninit::<libc::stat>::uninit();
        if unsafe {
            libc::fstatat(
                self.dir.as_raw_fd(),
                name.as_ptr(),
                metadata.as_mut_ptr(),
                libc::AT_SYMLINK_NOFOLLOW,
            )
        } != 0
        {
            return if std::io::Error::last_os_error().kind() == std::io::ErrorKind::NotFound {
                Ok(None)
            } else {
                Err("control_io")
            };
        }
        self.open(OsStr::from_bytes(name.as_bytes())).map(Some)
    }
    pub(super) fn write(&self, name: &str, bytes: &[u8]) -> Result<()> {
        let temporary = format!("{name}.helper-new");
        let mut file = open_at(
            &self.dir,
            OsStr::new(&temporary),
            libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL,
            0o600,
        )?;
        // SAFETY: the new held regular file belongs to this job. Giving the caller ownership
        // lets the unprivileged UI read the bounded receipt; no arbitrary existing file is changed.
        if unsafe { libc::fchown(file.as_raw_fd(), self.user.uid, self.user.gid) } != 0 {
            return Err("control_io");
        }
        file.write_all(bytes)
            .and_then(|()| file.sync_all())
            .map_err(|_| "control_io")?;
        let from = cstring(OsStr::new(&temporary))?;
        let to = cstring(OsStr::new(name))?;
        // SAFETY: both names are fixed receipt leaves under the same held job directory.
        if unsafe {
            libc::renameat(
                self.dir.as_raw_fd(),
                from.as_ptr(),
                self.dir.as_raw_fd(),
                to.as_ptr(),
            )
        } != 0
        {
            return Err("control_io");
        }
        Ok(())
    }
}

pub(super) fn authorize(request_path: &Path) -> Result<()> {
    // This entry point does not perform privileged work and never forwards arbitrary argv.
    let uid = unsafe { libc::geteuid() };
    if uid == 0
        || std::env::current_exe().map_err(|_| "unsupported_installation")?
            != Path::new("/usr/libexec/xdnote-update-helper")
    {
        return Err("unsupported_installation");
    }
    root_file(Path::new("/usr/libexec/xdnote-update-helper"))?;
    let job = Job::for_user(request_path, uid)?;
    let request: Request = serde_json::from_slice(&read_bounded(
        job.open(OsStr::new("request.json"))?,
        CONTROL_LIMIT,
    )?)
    .map_err(|_| "invalid_request")?;
    validate_request(&request)?;
    let status = Command::new("/usr/bin/pkexec")
        .arg("--disable-internal-agent")
        .arg("/usr/libexec/xdnote-update-helper")
        .arg("--request")
        .arg(request_path)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
    let error = match status {
        Ok(status) if status.success() => return Ok(()),
        Ok(status) if status.code() == Some(126) => "authorization_cancelled",
        Ok(_) => "authorization_failed",
        Err(_) => "authorization_unavailable",
    };
    if job.control("result.json")?.is_none() {
        write_control(&job, "result.json", &request.transaction, Some(error))?;
    }
    Err(error)
}

fn root_directory(path: &Path) -> Result<File> {
    let file = directory(path)?;
    for parent in path.ancestors() {
        let metadata = directory(parent)?
            .metadata()
            .map_err(|_| "unsafe_installation")?;
        if metadata.uid() != 0 || metadata.mode() & 0o022 != 0 {
            return Err("unsafe_installation");
        }
    }
    Ok(file)
}
fn root_file(path: &Path) -> Result<File> {
    let parent = root_directory(path.parent().ok_or("unsafe_installation")?)?;
    let file = open_at(
        &parent,
        path.file_name().ok_or("unsafe_installation")?,
        libc::O_RDONLY,
        0,
    )?;
    let metadata = file.metadata().map_err(|_| "unsafe_installation")?;
    if !metadata.is_file()
        || metadata.uid() != 0
        || metadata.mode() & 0o022 != 0
        || metadata.nlink() != 1
    {
        return Err("unsafe_installation");
    }
    Ok(file)
}

pub(super) fn installed_executable(request: &Request) -> Result<PathBuf> {
    if request.install_root.parent() != Some(Path::new("/opt/xdnote/versions")) {
        return Err("unsupported_installation");
    }
    root_directory(&request.install_root)?;
    let marker = read_bounded(
        root_file(&request.install_root.join("share/xdnote-install.json"))?,
        CONTROL_LIMIT,
    )?;
    let marker: serde_json::Value =
        serde_json::from_slice(&marker).map_err(|_| "unsupported_installation")?;
    if marker["schema_version"] != 1
        || marker["package_type"] != "deb"
        || marker["product"] != "xdnote"
    {
        return Err("unsupported_installation");
    }
    let executable = request.install_root.join("bin/xdnote");
    root_file(&executable)?;
    let owner = Command::new("/usr/bin/dpkg-query")
        .args(["-S"])
        .arg(&executable)
        .env_clear()
        .env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin")
        .env("LC_ALL", "C")
        .output()
        .map_err(|_| "package_identity")?;
    if !owner.status.success() || !String::from_utf8_lossy(&owner.stdout).starts_with("xdnote: ") {
        return Err("package_identity");
    }
    Ok(executable)
}
pub(super) fn updated_executable(_request: &Request) -> Result<PathBuf> {
    let launcher = read_bounded(root_file(Path::new("/usr/bin/xdnote"))?, CONTROL_LIMIT)?;
    let launcher = std::str::from_utf8(&launcher).map_err(|_| "installed_identity_mismatch")?;
    let prefix = "#!/bin/sh\nexec \"/opt/xdnote/versions/";
    let runtime = launcher
        .strip_prefix(prefix)
        .and_then(|text| text.strip_suffix("/bin/xdnote\" \"$@\"\n"))
        .filter(|text| {
            !text.is_empty() && text.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
        })
        .ok_or("installed_identity_mismatch")?;
    let executable = Path::new("/opt/xdnote/versions")
        .join(runtime)
        .join("bin/xdnote");
    root_file(&executable)?;
    Ok(executable)
}

pub(super) struct Parent {
    fd: File,
    environment: BTreeMap<String, String>,
}
fn desktop_environment(bytes: &[u8]) -> BTreeMap<String, String> {
    let mut environment = BTreeMap::new();
    for entry in bytes.split(|byte| *byte == 0) {
        if let Some((name, value)) = std::str::from_utf8(entry)
            .ok()
            .and_then(|text| text.split_once('='))
        {
            if [
                "DISPLAY",
                "XAUTHORITY",
                "DBUS_SESSION_BUS_ADDRESS",
                "XDG_RUNTIME_DIR",
                "XDG_CONFIG_HOME",
                "XDG_DATA_HOME",
                "XDG_CACHE_HOME",
                "XDG_STATE_HOME",
                "XDG_CONFIG_DIRS",
                "XDG_DATA_DIRS",
                "XDG_CURRENT_DESKTOP",
                "LANG",
                "LANGUAGE",
                "LC_ALL",
                "LC_MESSAGES",
                "LC_CTYPE",
                "LC_TIME",
            ]
            .contains(&name)
                && value.len() <= 4096
            {
                environment.insert(name.into(), value.into());
            }
        }
    }
    environment
}
impl Parent {
    pub(super) fn open(job: &Job, request: &Request) -> Result<Self> {
        // SAFETY: pidfd captures a process object and prevents a reused numeric PID from
        // being mistaken for the parent during the later non-mutating poll.
        let fd = unsafe { libc::syscall(libc::SYS_pidfd_open, request.parent_pid, 0) };
        if fd < 0 {
            return Err("parent_missing");
        }
        let fd = unsafe { File::from_raw_fd(fd as i32) };
        let proc = PathBuf::from(format!("/proc/{}", request.parent_pid));
        if fs::metadata(&proc).map_err(|_| "parent_identity")?.uid() != job.user.uid
            || fs::read_link(proc.join("exe")).map_err(|_| "parent_identity")?
                != request.install_root.join("bin/xdnote")
        {
            return Err("parent_identity");
        }
        let bytes = read_bounded(
            File::open(proc.join("environ")).map_err(|_| "parent_identity")?,
            128 * 1024,
        )?;
        let environment = desktop_environment(&bytes);
        let value = Self { fd, environment };
        if value.exited()? {
            return Err("parent_missing");
        }
        Ok(value)
    }
    pub(super) fn exited(&self) -> Result<bool> {
        let mut descriptor = libc::pollfd {
            fd: self.fd.as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        };
        // SAFETY: one live pidfd and initialized pollfd are borrowed for a zero-timeout query.
        let status = unsafe { libc::poll(&mut descriptor, 1, 0) };
        if status < 0 {
            return Err("parent_wait");
        }
        Ok(status > 0 && descriptor.revents & libc::POLLIN != 0)
    }
}

pub(super) struct Gate {
    _lock: File,
    path: PathBuf,
    transaction: String,
}
impl Gate {
    pub(super) fn acquire(request: &Request) -> Result<Self> {
        let dir = root_directory(Path::new("/run/xdnote"))?;
        let lock = open_at(
            &dir,
            OsStr::new("update.lock"),
            libc::O_RDWR | libc::O_CREAT,
            0o600,
        )?;
        let metadata = lock.metadata().map_err(|_| "update_gate")?;
        if metadata.uid() != 0 || metadata.mode() & 0o077 != 0 || metadata.nlink() != 1 {
            return Err("update_gate");
        }
        // SAFETY: this owned lock is nonblocking; no process or foreign lock is terminated.
        if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
            return Err("update_in_progress");
        }
        if Path::new("/run/xdnote/installing").exists() {
            return Err("installation_in_progress");
        }
        let path = PathBuf::from("/run/xdnote/updating");
        let mut pending = open_at(
            &dir,
            OsStr::new("updating"),
            libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL,
            0o644,
        )?;
        pending
            .write_all(request.transaction.as_bytes())
            .and_then(|()| pending.sync_all())
            .map_err(|_| "update_gate")?;
        Ok(Self {
            _lock: lock,
            path,
            transaction: request.transaction.clone(),
        })
    }
}
impl Drop for Gate {
    fn drop(&mut self) {
        // Only this transaction's root-owned marker is retired. A failed dpkg installing
        // marker is intentionally preserved so ordinary launches remain fail-closed.
        if fs::read(&self.path).ok().as_deref() == Some(self.transaction.as_bytes()) {
            let _ = fs::remove_file(&self.path);
        }
    }
}

pub(super) struct Staged {
    pub(super) inspection: xdnote_update::Inspection,
    pub(super) revision: String,
    package: PathBuf,
    _directory: StageDirectory,
}
struct StageDirectory {
    path: PathBuf,
    package: PathBuf,
}
impl Drop for StageDirectory {
    fn drop(&mut self) {
        // This root-owned, newly-created directory contains only the fixed verified package.
        // Remove known regular files individually, never recursively follow a supplied path.
        if root_file(&self.package).is_ok() {
            let _ = fs::remove_file(&self.package);
        }
        let _ = fs::remove_dir(&self.path);
    }
}
pub(super) fn stage(_job: &Job, request: &Request, mut prepared: Prepared) -> Result<Staged> {
    let base = Path::new("/var/cache/xdnote-update");
    if !base.exists() {
        fs::DirBuilder::new()
            .mode(0o755)
            .create(base)
            .map_err(|_| "stage_io")?;
    }
    root_directory(base)?;
    let stage = base.join(&request.transaction);
    fs::DirBuilder::new()
        .mode(0o755)
        .create(&stage)
        .map_err(|_| "stage_exists")?;
    let target = stage.join(&prepared.inspection.filename);
    let directory = StageDirectory {
        path: stage.clone(),
        package: target.clone(),
    };
    let mut output = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o644)
        .open(&target)
        .map_err(|_| "stage_io")?;
    let copied = std::io::copy(
        &mut Read::by_ref(&mut prepared.package).take(prepared.inspection.bytes + 1),
        &mut output,
    )
    .map_err(|_| "stage_io")?;
    output.sync_all().map_err(|_| "stage_io")?;
    if copied != prepared.inspection.bytes {
        return Err("package_size");
    }
    let mut package = root_file(&target)?;
    hash_package(
        &mut package,
        prepared.inspection.bytes,
        &prepared.inspection.sha256,
    )?;
    let envelope = std::str::from_utf8(&prepared.signature).map_err(|_| "invalid_signature")?;
    if !xdnote_update::verify_manifest(&prepared.manifest, envelope, TRUST).is_empty() {
        return Err("untrusted_signature");
    }
    let fields = Command::new("/usr/bin/dpkg-deb")
        .args(["--field"])
        .arg(&target)
        .args(["Package", "Architecture", "Version"])
        .env_clear()
        .env("PATH", "/usr/bin:/bin")
        .env("LC_ALL", "C")
        .output()
        .map_err(|_| "invalid_debian_package")?;
    let expected_version = format!(
        "1:{}+git{}",
        prepared.inspection.version.replacen('-', "~", 1),
        serde_json::from_slice::<serde_json::Value>(&prepared.manifest)
            .map_err(|_| "invalid_manifest")?["commit"]
            .as_str()
            .ok_or("invalid_manifest")?
            .get(..40)
            .ok_or("invalid_manifest")?
    );
    let expected = format!("Package: xdnote\nArchitecture: amd64\nVersion: {expected_version}\n");
    if !fields.status.success() || fields.stdout != expected.as_bytes() {
        return Err("invalid_debian_package");
    }
    Ok(Staged {
        inspection: prepared.inspection,
        revision: prepared.revision,
        package: target,
        _directory: directory,
    })
}
pub(super) fn install(_job: &Job, request: &Request, staged: &Staged) -> Result<()> {
    let status = Command::new("/usr/bin/apt-get")
        .args([
            "-y",
            "--no-remove",
            "--no-install-recommends",
            "-o",
            "Dpkg::Options::=--force-confold",
            "install",
        ])
        .arg(&staged.package)
        .env_clear()
        .env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin")
        .env("LC_ALL", "C")
        .env("DEBIAN_FRONTEND", "noninteractive")
        .env("XDNOTE_UPDATE_TRANSACTION", &request.transaction)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map_err(|_| "installer_start")?;
    if !status.success() {
        return Err("installation_failed");
    }
    let state = Command::new("/usr/bin/dpkg-query")
        .args(["-W", "-f=${Status}", "xdnote"])
        .env_clear()
        .env("PATH", "/usr/bin:/bin")
        .env("LC_ALL", "C")
        .output()
        .map_err(|_| "package_state")?;
    if !state.status.success() || state.stdout != b"install ok installed" {
        return Err("package_state");
    }
    Ok(())
}
pub(super) fn restart(job: &Job, _request: &Request, parent: &Parent) -> Result<()> {
    let mut command = Command::new("/usr/bin/setpriv");
    command
        .args([
            "--reuid",
            &job.user.uid.to_string(),
            "--regid",
            &job.user.gid.to_string(),
            "--init-groups",
            "--",
            "/usr/bin/xdnote",
        ])
        .env_clear()
        .env("PATH", "/usr/local/bin:/usr/bin:/bin")
        .env("HOME", &job.user.home)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    for (name, value) in &parent.environment {
        command.env(name, value);
    }
    command.spawn().map(|_| ()).map_err(|_| "restart_failed")
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn restart_preserves_user_configuration_without_loader_overrides() {
        let environment = desktop_environment(
            b"XDG_CONFIG_HOME=/owned/config\0XDG_DATA_HOME=/owned/data\0\
              XDG_CACHE_HOME=/owned/cache\0XDG_STATE_HOME=/owned/state\0\
              LC_ALL=zh_CN.UTF-8\0DISPLAY=:9\0LD_PRELOAD=/untrusted/loader.so\0\
              QT_PLUGIN_PATH=/untrusted/plugins\0XDNOTE_SETTINGS_PATH=/test-only.ini\0",
        );
        assert_eq!(environment["XDG_CONFIG_HOME"], "/owned/config");
        assert_eq!(environment["XDG_DATA_HOME"], "/owned/data");
        assert_eq!(environment["XDG_CACHE_HOME"], "/owned/cache");
        assert_eq!(environment["XDG_STATE_HOME"], "/owned/state");
        assert_eq!(environment["LC_ALL"], "zh_CN.UTF-8");
        assert_eq!(environment["DISPLAY"], ":9");
        assert!(!environment.contains_key("LD_PRELOAD"));
        assert!(!environment.contains_key("QT_PLUGIN_PATH"));
        assert!(!environment.contains_key("XDNOTE_SETTINGS_PATH"));
        let oversized = format!("XDG_CONFIG_HOME={}", "x".repeat(4097));
        assert!(desktop_environment(oversized.as_bytes()).is_empty());
    }
    fn fixture() -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "xdnote-linux-job-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
        path
    }
    #[test]
    fn job_rejects_symlink_sources_and_broad_directory_permissions() {
        use std::os::unix::fs::{symlink, PermissionsExt};
        let root = fixture();
        let original = root.join("original");
        fs::write(&original, b"owned").unwrap();
        symlink(&original, root.join("source")).unwrap();
        let job = Job::for_user(&root.join("request.json"), unsafe { libc::geteuid() }).unwrap();
        assert!(job.open(OsStr::new("source")).is_err());
        fs::set_permissions(&root, fs::Permissions::from_mode(0o755)).unwrap();
        assert!(Job::for_user(&root.join("request.json"), unsafe { libc::geteuid() }).is_err());
        fs::remove_file(root.join("source")).unwrap();
        fs::remove_file(original).unwrap();
        fs::remove_dir(root).unwrap();
    }
    #[test]
    fn held_job_directory_and_atomic_receipts_do_not_follow_replacement() {
        let root = fixture();
        fs::write(root.join("source"), b"original").unwrap();
        let job = Job::for_user(&root.join("request.json"), unsafe { libc::geteuid() }).unwrap();
        let moved = root.with_extension("moved");
        fs::rename(&root, &moved).unwrap();
        fs::DirBuilder::new().mode(0o700).create(&root).unwrap();
        fs::write(root.join("source"), b"replacement").unwrap();
        assert_eq!(
            read_bounded(job.open(OsStr::new("source")).unwrap(), 32).unwrap(),
            b"original"
        );
        write_control(&job, "ready.json", &"a".repeat(32), None).unwrap();
        assert!(!root.join("ready.json").exists());
        assert!(control(&job, "ready.json", &"a".repeat(32)).unwrap());
        assert_eq!(
            control(&job, "ready.json", &"b".repeat(32)),
            Err("transaction_mismatch")
        );
        fs::remove_file(root.join("source")).unwrap();
        fs::remove_dir(root).unwrap();
        fs::remove_file(moved.join("source")).unwrap();
        fs::remove_file(moved.join("ready.json")).unwrap();
        fs::remove_dir(moved).unwrap();
    }
}
