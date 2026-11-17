// crates/xdnote-update-helper/src/windows.rs

//! Held Windows handles protect verified inputs while the interactive installer runs.

use super::*;
use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::fs::{MetadataExt, OpenOptionsExt};
use std::sync::Arc;
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_ALREADY_EXISTS, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows_sys::Win32::Storage::FileSystem::{
    FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT, FILE_SHARE_READ, FILE_SHARE_WRITE,
};
use windows_sys::Win32::System::Registry::{RegGetValueW, HKEY_CURRENT_USER, RRF_RT_REG_SZ};
use windows_sys::Win32::System::Threading::{
    CreateMutexW, OpenProcess, QueryFullProcessImageNameW, WaitForSingleObject,
    PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SYNCHRONIZE,
};

pub(super) const PLATFORM: &str = "windows";
pub(super) const PACKAGE: &str = "exe";
const INSTALL_ID: &str = "460D7C68-C989-45F0-B312-40AE18428155";

fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

struct Handle(HANDLE);
impl Drop for Handle {
    fn drop(&mut self) {
        // SAFETY: this owner only receives successful, owned native handles.
        unsafe {
            CloseHandle(self.0);
        }
    }
}

struct DirectoryLease {
    _ancestors: Vec<File>,
}
impl DirectoryLease {
    fn for_creation(path: &Path) -> Result<Self> {
        if !path.is_absolute() {
            return Err("unsafe_job_directory");
        }
        let mut current = PathBuf::new();
        let mut ancestors = Vec::new();
        for part in path.components() {
            match part {
                std::path::Component::Prefix(prefix)
                    if matches!(
                        prefix.kind(),
                        std::path::Prefix::Disk(_) | std::path::Prefix::VerbatimDisk(_)
                    ) =>
                {
                    current.push(prefix.as_os_str())
                }
                std::path::Component::RootDir => current.push(part.as_os_str()),
                std::path::Component::Normal(name) => {
                    current.push(name);
                    let file = fs::OpenOptions::new()
                        .read(true)
                        .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
                        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
                        .open(&current)
                        .map_err(|_| "unsafe_job_directory")?;
                    let meta = file.metadata().map_err(|_| "unsafe_job_directory")?;
                    if !meta.is_dir() || meta.file_attributes() & 0x400 != 0 {
                        return Err("unsafe_job_directory");
                    }
                    ancestors.push(file);
                }
                _ => return Err("unsafe_job_directory"),
            }
        }
        Ok(Self {
            _ancestors: ancestors,
        })
    }
    fn validate(&self) -> Result<()> {
        if self._ancestors.is_empty() {
            Err("unsafe_job_directory")
        } else {
            Ok(())
        }
    }
}

pub(super) struct Job {
    path: PathBuf,
    grant: Arc<DirectoryLease>,
    _directory: File,
}
impl Job {
    pub(super) fn load(request: &Path) -> Result<Self> {
        if !request.is_absolute() || request.file_name() != Some(OsStr::new("request.json")) {
            return Err("invalid_request_path");
        }
        let path = request
            .parent()
            .ok_or("invalid_request_path")?
            .to_path_buf();
        let grant = DirectoryLease::for_creation(&path).map_err(|_| "unsafe_job_directory")?;
        let directory = fs::OpenOptions::new()
            .read(true)
            // Permit receipt creation/rename inside this held directory. Denying
            // FILE_SHARE_DELETE still prevents the job directory from moving.
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
            .custom_flags(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
            .open(&path)
            .map_err(|_| "unsafe_job_directory")?;
        if directory
            .metadata()
            .map_err(|_| "unsafe_job_directory")?
            .file_attributes()
            & 0x400
            != 0
        {
            return Err("unsafe_job_directory");
        }
        Ok(Self {
            path,
            grant: Arc::new(grant),
            _directory: directory,
        })
    }
    pub(super) fn path(&self) -> &Path {
        &self.path
    }
    pub(super) fn open(&self, name: &OsStr) -> Result<File> {
        self.grant.validate().map_err(|_| "job_directory_changed")?;
        let path = self.path.join(name);
        if path.parent() != Some(&self.path) {
            return Err("invalid_request_path");
        }
        stable_file(&path)
    }
    pub(super) fn control(&self, name: &str) -> Result<Option<File>> {
        match fs::symlink_metadata(self.path.join(name)) {
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(_) => Err("control_io"),
            Ok(_) => self.open(OsStr::new(name)).map(Some),
        }
    }
    pub(super) fn write(&self, name: &str, bytes: &[u8]) -> Result<()> {
        self.grant.validate().map_err(|_| "job_directory_changed")?;
        let temporary = self.path.join(format!("{name}.helper-new"));
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .map_err(|_| "control_io")?;
        file.write_all(bytes)
            .and_then(|()| file.sync_all())
            .map_err(|_| "control_io")?;
        drop(file);
        fs::rename(temporary, self.path.join(name)).map_err(|_| "control_io")
    }
}

fn stable_file(path: &Path) -> Result<File> {
    let file = fs::OpenOptions::new()
        .read(true)
        .share_mode(FILE_SHARE_READ)
        .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT)
        .open(path)
        .map_err(|_| "source_io")?;
    let metadata = file.metadata().map_err(|_| "source_io")?;
    if !metadata.is_file() || metadata.file_attributes() & 0x400 != 0 {
        return Err("unsafe_source");
    }
    Ok(file)
}

fn registry(name: &str) -> Result<String> {
    let key = wide("Software\\xdnote\\Install");
    let name = wide(name);
    let mut buffer = [0u16; 32768];
    let mut length = std::mem::size_of_val(&buffer) as u32;
    // SAFETY: all strings are NUL terminated; buffer and length are valid outputs.
    let status = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            key.as_ptr(),
            name.as_ptr(),
            RRF_RT_REG_SZ,
            std::ptr::null_mut(),
            buffer.as_mut_ptr().cast(),
            &mut length,
        )
    };
    if status != 0 || length < 2 || length as usize > std::mem::size_of_val(&buffer) {
        return Err("unsupported_installation");
    }
    let length = length as usize / 2;
    if buffer[length - 1] != 0 {
        return Err("unsupported_installation");
    }
    String::from_utf16(&buffer[..length - 1]).map_err(|_| "unsupported_installation")
}

fn equal_path(left: &Path, right: &Path) -> bool {
    match (fs::canonicalize(left), fs::canonicalize(right)) {
        (Ok(left), Ok(right)) => left
            .to_string_lossy()
            .eq_ignore_ascii_case(&right.to_string_lossy()),
        _ => false,
    }
}

pub(super) fn installed_executable(request: &Request) -> Result<PathBuf> {
    if registry("InstallId")? != INSTALL_ID
        || !equal_path(Path::new(&registry("Location")?), &request.install_root)
    {
        return Err("unsupported_installation");
    }
    DirectoryLease::for_creation(&request.install_root).map_err(|_| "unsafe_installation")?;
    let manifest = fs::symlink_metadata(request.install_root.join("install-manifest.txt"))
        .map_err(|_| "unsupported_installation")?;
    if !manifest.is_file() || manifest.file_attributes() & 0x400 != 0 {
        return Err("unsafe_installation");
    }
    Ok(request.install_root.join("xdnote.exe"))
}
pub(super) fn updated_executable(request: &Request) -> Result<PathBuf> {
    installed_executable(request)
}

pub(super) struct Parent(Handle);
impl Parent {
    pub(super) fn open(_job: &Job, request: &Request) -> Result<Self> {
        // SAFETY: OpenProcess returns an owned handle or NULL; no access grants process mutation.
        let process = unsafe {
            OpenProcess(
                PROCESS_SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                0,
                request.parent_pid,
            )
        };
        if process.is_null() {
            return Err("parent_missing");
        }
        let process = Handle(process);
        let mut path = [0u16; 32768];
        let mut length = path.len() as u32;
        // SAFETY: the live process handle is queried into a bounded initialized buffer.
        if unsafe { QueryFullProcessImageNameW(process.0, 0, path.as_mut_ptr(), &mut length) } == 0
        {
            return Err("parent_identity");
        }
        let path = String::from_utf16(&path[..length as usize]).map_err(|_| "parent_identity")?;
        if !equal_path(Path::new(&path), &request.install_root.join("xdnote.exe")) {
            return Err("parent_identity");
        }
        Ok(Self(process))
    }
    pub(super) fn exited(&self) -> Result<bool> {
        // SAFETY: waiting with zero timeout only observes this held process object.
        match unsafe { WaitForSingleObject(self.0 .0, 0) } {
            WAIT_OBJECT_0 => Ok(true),
            WAIT_TIMEOUT => Ok(false),
            _ => Err("parent_wait"),
        }
    }
}

pub(super) struct Gate {
    _main: Handle,
    _transaction: Handle,
}
impl Gate {
    pub(super) fn acquire(request: &Request) -> Result<Self> {
        fn create(name: &str) -> Result<Handle> {
            let name = wide(name);
            // SAFETY: name is terminated, default token ACL applies, handle ownership is retained.
            let handle = unsafe { CreateMutexW(std::ptr::null(), 0, name.as_ptr()) };
            if handle.is_null() {
                return Err("update_gate");
            }
            let handle = Handle(handle);
            // SAFETY: captured immediately after CreateMutex; existing objects reject parallel updates.
            if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
                return Err("update_in_progress");
            }
            Ok(handle)
        }
        let main = create("Local\\xdnote.desktop.updating")?;
        let transaction = create(&format!(
            "Local\\xdnote.desktop.update.{}",
            request.transaction
        ))?;
        Ok(Self {
            _main: main,
            _transaction: transaction,
        })
    }
}

pub(super) struct Staged {
    pub(super) inspection: xdnote_update::Inspection,
    pub(super) revision: String,
    _package: File,
}
pub(super) fn stage(_job: &Job, _request: &Request, prepared: Prepared) -> Result<Staged> {
    // The read-only, no-write/no-delete-sharing package handle remains live through Setup.
    Ok(Staged {
        inspection: prepared.inspection,
        revision: prepared.revision,
        _package: prepared.package,
    })
}
pub(super) fn install(_job: &Job, request: &Request, _staged: &Staged) -> Result<()> {
    let status = Command::new(&request.package_file)
        .arg("/NORESTART")
        .arg("/RESTARTEXITCODE=3010")
        .arg("/NOCLOSEAPPLICATIONS")
        .arg("/NORESTARTAPPLICATIONS")
        .arg(format!("/XDNOTEUPDATE={}", request.transaction))
        .arg(format!("/DIR={}", request.install_root.display()))
        .stdin(Stdio::null())
        .status()
        .map_err(|_| "installer_start")?;
    match status.code() {
        Some(0) => Ok(()),
        Some(2 | 5) => Err("installation_cancelled"),
        Some(20) => Err("cleanup_incomplete"),
        Some(3010) => Err("restart_required"),
        _ => Err("installation_failed"),
    }
}
pub(super) fn restart(_job: &Job, request: &Request, _parent: &Parent) -> Result<()> {
    Command::new(installed_executable(request)?)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map(|_| ())
        .map_err(|_| "restart_failed")
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn held_job_directory_allows_atomic_receipts_but_refuses_directory_move() {
        let path = std::env::temp_dir().join(format!(
            "xdnote-job-control-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&path).unwrap();
        let acl = Command::new("icacls.exe")
            .arg(&path)
            .args(["/inheritance:r", "/grant:r", "*S-1-3-4:(OI)(CI)F"])
            .output()
            .unwrap();
        assert!(acl.status.success());
        let job = Job::load(&path.join("request.json")).unwrap();
        job.write("ready.json", br#"{"transaction":"owned"}"#)
            .unwrap();
        assert_eq!(
            fs::read(path.join("ready.json")).unwrap(),
            br#"{"transaction":"owned"}"#
        );
        assert!(fs::rename(&path, path.with_extension("moved")).is_err());
        drop(job);
        fs::remove_file(path.join("ready.json")).unwrap();
        fs::remove_dir(path).unwrap();
    }

    #[test]
    fn verified_package_handle_refuses_replacement_and_concurrent_writes() {
        let path = std::env::temp_dir().join(format!(
            "xdnote-held-package-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::write(&path, b"owned fixture").unwrap();
        let held = stable_file(&path).unwrap();
        assert!(fs::OpenOptions::new().write(true).open(&path).is_err());
        assert!(fs::remove_file(&path).is_err());
        assert_eq!(read_bounded(held, 32).unwrap(), b"owned fixture");
        fs::remove_file(&path).unwrap();
    }
}
