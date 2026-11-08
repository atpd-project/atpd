use super::SupervisorError;
use std::ffi::CString;
use std::fs::File;
use std::io::{self, Write};
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Command, Stdio};

pub fn service_args_have_custom_config(value: &str) -> Result<bool, SupervisorError> {
    if value.contains('\'') || value.contains('"') || value.contains('\\') {
        return Err(SupervisorError::ConfigValidationFailed(
            "SERVICE_ARGS does not support shell quoting or escaping".to_string(),
        ));
    }

    for token in value.split_ascii_whitespace() {
        if token == "-c" || token == "-C" {
            return Ok(true);
        }
    }

    Ok(false)
}

pub fn build_service_args(
    bin_path: &str,
    work_dir: &str,
    conf_path: &str,
    service_args: &str,
) -> Result<Vec<String>, SupervisorError> {
    let has_custom_c = service_args_have_custom_config(service_args)?;

    let mut args = Vec::with_capacity(30);
    args.push(bin_path.to_string());
    args.push("run".to_string());

    if !work_dir.is_empty() {
        args.push("-D".to_string());
        args.push(work_dir.to_string());
    }

    if !has_custom_c && !conf_path.is_empty() {
        args.push("-c".to_string());
        args.push(conf_path.to_string());
    }

    if !service_args.is_empty() {
        for token in service_args.split_ascii_whitespace() {
            if args.len() < 28 {
                args.push(token.to_string());
            }
        }
    }

    Ok(args)
}

pub fn parse_service_env(env_str: &str) -> Result<Vec<(String, String)>, SupervisorError> {
    let trimmed = env_str.trim();
    if trimmed.is_empty() {
        return Ok(Vec::new());
    }

    let mut env_pairs = Vec::new();
    for token in trimmed.split_ascii_whitespace() {
        let eq_pos = match token.find('=') {
            Some(pos) if pos > 0 => pos,
            _ => {
                return Err(SupervisorError::ConfigValidationFailed(format!(
                    "invalid SERVICE_ENV token '{}' (expected KEY=VALUE)",
                    token
                )));
            }
        };

        let key = &token[..eq_pos];
        let val = &token[eq_pos + 1..];

        let mut chars = key.chars();
        let first = chars.next().unwrap_or('\0');
        if !first.is_ascii_alphabetic() && first != '_' {
            return Err(SupervisorError::ConfigValidationFailed(format!(
                "invalid SERVICE_ENV key in token '{}'",
                token
            )));
        }

        for c in chars {
            if !c.is_ascii_alphanumeric() && c != '_' {
                return Err(SupervisorError::ConfigValidationFailed(format!(
                    "invalid SERVICE_ENV key in token '{}'",
                    token
                )));
            }
        }

        env_pairs.push((key.to_string(), val.to_string()));
    }

    Ok(env_pairs)
}

pub fn open_regular_file(path: &Path, flags: libc::c_int, mode: libc::mode_t) -> io::Result<File> {
    let oflags = rustix::fs::OFlags::from_bits_retain(flags as u32)
        | rustix::fs::OFlags::NOFOLLOW
        | rustix::fs::OFlags::CLOEXEC;
    let file_mode = rustix::fs::Mode::from_raw_mode(mode as _);
    let fd = rustix::fs::openat(rustix::fs::CWD, path, oflags, file_mode)?;
    let st = rustix::fs::fstat(&fd)?;
    if (st.st_mode as libc::mode_t & libc::S_IFMT) != libc::S_IFREG || st.st_nlink != 1 {
        return Err(io::Error::from_raw_os_error(libc::EINVAL));
    }
    Ok(File::from(fd))
}

const SERVICE_LOG_MAX_BYTES: u64 = 10 * 1024 * 1024;
const SERVICE_LOG_KEEP: usize = 3;

/// Copy an oversized log to `.1` (keeping 3 copies) and empty it in place.
/// Truncating instead of renaming lets a running sing-box keep its O_APPEND fd.
pub fn service_rotate_log(log_path: &Path) {
    let size = match std::fs::metadata(log_path) {
        Ok(m) => m.len(),
        Err(_) => return,
    };
    if size < SERVICE_LOG_MAX_BYTES {
        return;
    }
    let base = log_path.to_string_lossy();
    let tmp_path = format!("{}.tmp", base);
    if let Err(e) = std::fs::copy(log_path, &tmp_path) {
        crate::log_warn!("Service: failed to rotate log: {}", e);
        let _ = std::fs::remove_file(&tmp_path);
        return;
    }
    crate::logger::shift_backups(&base, SERVICE_LOG_KEEP);
    // Truncate only once the copy is safely in .1, or its contents are lost.
    let res = std::fs::rename(&tmp_path, format!("{}.1", base))
        .and_then(|_| std::fs::OpenOptions::new().write(true).open(log_path))
        .and_then(|f| f.set_len(0));
    if res.is_err() {
        let _ = std::fs::remove_file(&tmp_path);
    }
    match res {
        Ok(()) => crate::log_info!("Service: rotated log to {}.1 (size: {} bytes)", base, size),
        Err(e) => crate::log_warn!("Service: failed to rotate log: {}", e),
    }
}

pub fn write_pid_file(pid_path: &Path, pid: i32) -> Result<(), SupervisorError> {
    if let Some(parent) = pid_path.parent() {
        std::fs::create_dir_all(parent).map_err(SupervisorError::Io)?;
    }

    let file = open_regular_file(pid_path, libc::O_WRONLY | libc::O_CREAT, 0o644)
        .map_err(SupervisorError::Io)?;

    file.set_len(0).map_err(SupervisorError::Io)?;
    let mut writer = file;
    writeln!(writer, "{}", pid).map_err(SupervisorError::Io)?;
    writer.sync_all().map_err(SupervisorError::Io)?;
    Ok(())
}

pub fn unlink_pid_file(pid_path: &Path) {
    let _ = std::fs::remove_file(pid_path);
}

pub fn service_is_alive(pid: i32, expected_starttime: u64) -> bool {
    if pid <= 0 || expected_starttime == 0 {
        return false;
    }

    let exists = match rustix::process::Pid::from_raw(pid) {
        Some(p) => matches!(
            rustix::process::test_kill_process(p),
            Ok(()) | Err(rustix::io::Errno::PERM)
        ),
        None => false,
    };
    if !exists {
        return false;
    }

    match crate::utils::proc::process_starttime(pid) {
        Ok(st) => st == expected_starttime,
        Err(_) => false,
    }
}

pub fn validate_process(pid: i32, expected_starttime: u64, bin_path: &str) -> bool {
    if pid <= 0 {
        return false;
    }

    let exists = match rustix::process::Pid::from_raw(pid) {
        Some(p) => matches!(
            rustix::process::test_kill_process(p),
            Ok(()) | Err(rustix::io::Errno::PERM)
        ),
        None => false,
    };
    if !exists {
        return false;
    }

    let current_starttime = match crate::utils::proc::process_starttime(pid) {
        Ok(st) => st,
        Err(_) => return false,
    };
    if expected_starttime != 0 && current_starttime != expected_starttime {
        return false;
    }

    let exe_path = format!("/proc/{}/exe", pid);
    if let Ok(target) = std::fs::read_link(&exe_path) {
        if let (Some(target_name), Some(bin_name)) =
            (target.file_name(), Path::new(bin_path).file_name())
        {
            if target_name == bin_name {
                return true;
            }
        }
    }

    let comm_path = format!("/proc/{}/comm", pid);
    if let Ok(comm_content) = std::fs::read_to_string(&comm_path) {
        if comm_content.trim() == "sing-box" {
            return true;
        }
    }

    false
}

fn resolve_supplementary_groups(user: &str, gid: libc::gid_t) -> io::Result<Vec<libc::gid_t>> {
    let c_user = CString::new(user).map_err(|_| io::Error::from_raw_os_error(libc::EINVAL))?;
    let mut ngroups: libc::c_int = 32;
    let mut groups: Vec<libc::gid_t> = vec![0; ngroups as usize];

    // SAFETY: getgrouplist is called in the parent before spawning.
    let ret =
        unsafe { libc::getgrouplist(c_user.as_ptr(), gid, groups.as_mut_ptr(), &mut ngroups) };
    if ret < 0 {
        groups.resize(ngroups as usize, 0);
        // SAFETY: second getgrouplist call with buffer resized to ngroups.
        let ret2 =
            unsafe { libc::getgrouplist(c_user.as_ptr(), gid, groups.as_mut_ptr(), &mut ngroups) };
        if ret2 < 0 {
            return Err(io::Error::last_os_error());
        }
    }
    groups.truncate(ngroups as usize);
    Ok(groups)
}

#[allow(clippy::too_many_arguments)]
pub fn service_spawn(
    bin_path: &str,
    work_dir: &str,
    conf_path: &str,
    log_path: &str,
    pid_path: &str,
    user: &str,
    group: &str,
    service_args: &str,
    service_env: &str,
) -> Result<(i32, u64), SupervisorError> {
    if !Path::new(bin_path).is_file() {
        return Err(SupervisorError::BinaryNotFound(bin_path.to_string()));
    }

    service_rotate_log(Path::new(log_path));

    if !work_dir.is_empty() {
        let _ = std::fs::create_dir_all(work_dir);
    }
    if let Some(parent) = Path::new(pid_path).parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let default_uid = rustix::process::getuid().as_raw();
    let default_gid = rustix::process::getgid().as_raw();
    let is_root = rustix::process::geteuid().as_raw() == 0;

    let creds = crate::supervisor::credentials::service_credentials_resolve(
        Some(user),
        Some(group),
        default_uid,
        default_gid,
    )?;

    let supp_groups = if is_root && creds.init_groups {
        resolve_supplementary_groups(&creds.user_name, creds.gid).ok()
    } else {
        None
    };

    let args = build_service_args(bin_path, work_dir, conf_path, service_args)?;
    let env_vars = parse_service_env(service_env)?;

    let log_path_ref = Path::new(log_path);
    let (stdout_stdio, stderr_stdio) = match open_regular_file(
        log_path_ref,
        libc::O_WRONLY | libc::O_CREAT | libc::O_APPEND,
        0o640,
    ) {
        Ok(file) => {
            let err_file = file.try_clone().ok();
            (Some(Stdio::from(file)), err_file.map(Stdio::from))
        }
        Err(e) => {
            crate::log_warn!(
                "Service: cannot open {}: {}; sing-box output is discarded",
                log_path,
                e
            );
            (None, None)
        }
    };

    let mut empty_sigmask = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
    // SAFETY: sigemptyset initializes an empty signal set in the parent.
    unsafe { libc::sigemptyset(empty_sigmask.as_mut_ptr()) };
    // SAFETY: sigemptyset initialized empty_sigmask.
    let empty_sigmask = unsafe { empty_sigmask.assume_init() };

    let mut cmd = Command::new(bin_path);
    cmd.args(&args[1..]);

    if !work_dir.is_empty() {
        cmd.current_dir(work_dir);
    }

    cmd.env_remove("LD_PRELOAD");
    for (k, v) in env_vars {
        cmd.env(k, v);
    }

    cmd.stdin(Stdio::null());
    if let Some(out) = stdout_stdio {
        cmd.stdout(out);
    } else {
        cmd.stdout(Stdio::null());
    }
    if let Some(err) = stderr_stdio {
        cmd.stderr(err);
    } else {
        cmd.stderr(Stdio::null());
    }

    let drop_privileges = is_root;
    let target_uid = creds.uid;
    let target_gid = creds.gid;

    // SAFETY: pre_exec is an unsafe method on CommandExt that configures process execution before exec.
    unsafe {
        cmd.pre_exec(move || {
            // Same as service.c: files sing-box creates are not world-readable.
            libc::umask(0o027);
            if libc::setsid() < 0 {
                return Err(io::Error::last_os_error());
            }

            if libc::sigprocmask(libc::SIG_SETMASK, &empty_sigmask, std::ptr::null_mut()) < 0 {
                return Err(io::Error::last_os_error());
            }

            if drop_privileges {
                if let Some(ref groups) = supp_groups {
                    if libc::setgroups(groups.len() as libc::size_t, groups.as_ptr()) < 0 {
                        return Err(io::Error::last_os_error());
                    }
                } else if libc::setgroups(0, std::ptr::null()) < 0 {
                    return Err(io::Error::last_os_error());
                }

                if libc::setresgid(target_gid, target_gid, target_gid) < 0 {
                    return Err(io::Error::last_os_error());
                }

                if libc::setresuid(target_uid, target_uid, target_uid) < 0 {
                    return Err(io::Error::last_os_error());
                }
            }

            Ok(())
        });
    }

    let child = cmd.spawn().map_err(SupervisorError::Io)?;
    let pid = child.id() as i32;

    let starttime = match crate::utils::proc::process_starttime(pid) {
        Ok(st) => st,
        Err(_) => {
            if let Some(target) = rustix::process::Pid::from_raw(pid) {
                let _ = rustix::process::kill_process(target, rustix::process::Signal::KILL);
                while let Err(rustix::io::Errno::INTR) =
                    rustix::process::waitpid(Some(target), rustix::process::WaitOptions::empty())
                {
                }
            }
            return Err(SupervisorError::SpawnFailed(
                "failed to capture child starttime".to_string(),
            ));
        }
    };

    if let Err(e) = write_pid_file(Path::new(pid_path), pid) {
        if let Some(target) = rustix::process::Pid::from_raw(pid) {
            let _ = rustix::process::kill_process(target, rustix::process::Signal::KILL);
            while let Err(rustix::io::Errno::INTR) =
                rustix::process::waitpid(Some(target), rustix::process::WaitOptions::empty())
            {
            }
        }
        unlink_pid_file(Path::new(pid_path));
        return Err(e);
    }

    Ok((pid, starttime))
}

pub fn service_validate_config(
    bin_path: &str,
    work_dir: &str,
    conf_path: &str,
    service_args: &str,
) -> Result<(), SupervisorError> {
    if !Path::new(bin_path).is_file() {
        return Err(SupervisorError::BinaryNotFound(bin_path.to_string()));
    }

    let mut args = build_service_args(bin_path, work_dir, conf_path, service_args)?;
    if args.len() > 1 {
        args[1] = "check".to_string();
    }

    let mut cmd = Command::new(bin_path);
    cmd.args(&args[1..]);
    if !work_dir.is_empty() {
        cmd.current_dir(work_dir);
    }
    cmd.env_remove("LD_PRELOAD");
    cmd.stdin(Stdio::null());
    cmd.stdout(Stdio::piped());
    cmd.stderr(Stdio::piped());

    let mut child = cmd.spawn().map_err(SupervisorError::Io)?;
    let start = std::time::Instant::now();
    let timeout = std::time::Duration::from_secs(5);

    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                if status.success() {
                    return Ok(());
                } else {
                    return Err(SupervisorError::ConfigValidationFailed(format!(
                        "sing-box check exited with status {}",
                        status
                    )));
                }
            }
            Ok(None) => {
                if start.elapsed() >= timeout {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(SupervisorError::ConfigValidationFailed(
                        "sing-box check timed out".to_string(),
                    ));
                }
                std::thread::sleep(std::time::Duration::from_millis(50));
            }
            Err(e) => return Err(SupervisorError::Io(e)),
        }
    }
}
