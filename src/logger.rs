//! Process-wide logger implementation for ATPD.

use std::fs::File;
use std::io::Write;
use std::os::unix::io::FromRawFd;
use std::sync::Mutex;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(i32)]
pub enum LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
}

impl LogLevel {
    pub fn as_str(&self) -> &'static str {
        match self {
            LogLevel::Debug => "DEBUG",
            LogLevel::Info => "INFO",
            LogLevel::Warn => "WARN",
            LogLevel::Error => "ERROR",
        }
    }

    pub fn color(&self) -> &'static str {
        match self {
            LogLevel::Debug => "\x1b[36m",
            LogLevel::Info => "\x1b[32m",
            LogLevel::Warn => "\x1b[33m",
            LogLevel::Error => "\x1b[31m",
        }
    }

    pub fn is_emittable(&self) -> bool {
        matches!(
            self,
            LogLevel::Debug | LogLevel::Info | LogLevel::Warn | LogLevel::Error
        )
    }

    #[cfg(target_os = "android")]
    pub fn android_prio(&self) -> i32 {
        match self {
            LogLevel::Debug => 3, // ANDROID_LOG_DEBUG
            LogLevel::Info => 4,  // ANDROID_LOG_INFO
            LogLevel::Warn => 5,  // ANDROID_LOG_WARN
            LogLevel::Error => 6, // ANDROID_LOG_ERROR
        }
    }
}

/// Shift `base.1 .. base.{keep-1}` up by one, dropping the oldest, so `base.1` is free.
pub fn shift_backups(base: &str, keep: usize) {
    for i in (2..=keep).rev() {
        let _ = std::fs::rename(format!("{}.{}", base, i - 1), format!("{}.{}", base, i));
    }
}

pub const LOG_TARGET_STDERR: u32 = 1 << 0;
pub const LOG_TARGET_FILE: u32 = 1 << 1;
pub const COLOR_RESET: &str = "\x1b[0m";

struct LoggerState {
    min_level: LogLevel,
    targets: u32,
    log_file: String,
    enable_color: bool,
    max_file_size: usize,
    rotate_count: usize,
    file: Option<File>,
    current_size: usize,
}

impl LoggerState {
    fn close_file(&mut self) {
        if let Some(mut f) = self.file.take() {
            let _ = f.flush();
        }
        self.current_size = 0;
    }

    fn open_file(&mut self) {
        if self.log_file.is_empty() || self.file.is_some() {
            return;
        }

        let c_path = match std::ffi::CString::new(self.log_file.as_bytes()) {
            Ok(p) => p,
            Err(_) => return,
        };

        // SAFETY: Opening file with O_NOFOLLOW to avoid symlink traversal.
        let fd = unsafe {
            libc::open(
                c_path.as_ptr(),
                libc::O_WRONLY
                    | libc::O_CREAT
                    | libc::O_APPEND
                    | libc::O_NOFOLLOW
                    | libc::O_CLOEXEC,
                0o640,
            )
        };
        if fd < 0 {
            return;
        }

        // SAFETY: zeroed stat buffer for fstat call.
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        // SAFETY: fd is a valid open file descriptor and st is a valid pointer.
        let stat_res = unsafe { libc::fstat(fd, &mut st) };
        if stat_res != 0 {
            // SAFETY: closing fd after stat failure.
            unsafe { libc::close(fd) };
            return;
        }

        let is_reg = (st.st_mode & libc::S_IFMT) == libc::S_IFREG;
        let is_single_link = st.st_nlink == 1;

        if !is_reg || !is_single_link {
            // SAFETY: closing fd because target is not a regular file or has hardlinks.
            unsafe { libc::close(fd) };
            return;
        }

        // SAFETY: fd is valid, single-link regular file, and we transfer ownership to File.
        let file = unsafe { File::from_raw_fd(fd) };
        self.current_size = st.st_size as usize;
        self.file = Some(file);
    }

    fn rotate_file(&mut self) {
        if self.log_file.is_empty() {
            return;
        }

        self.close_file();

        shift_backups(&self.log_file, self.rotate_count - 1);
        let _ = std::fs::rename(&self.log_file, format!("{}.1", self.log_file));

        self.open_file();
    }

    fn write_to_file(&mut self, level: LogLevel, msg: &str, ts: &str) {
        if self.file.is_none() {
            self.open_file();
            if self.file.is_none() {
                return;
            }
        }

        let line = format!("[{}] {}: {}\n", ts, level.as_str(), msg);
        let bytes = line.as_bytes();

        if let Some(ref mut f) = self.file {
            if f.write_all(bytes).is_ok() {
                let _ = f.flush();
                self.current_size = self.current_size.saturating_add(bytes.len());
                if self.current_size >= self.max_file_size {
                    self.rotate_file();
                }
            }
        }
    }

    fn write_to_stderr(&self, level: LogLevel, msg: &str, ts: &str) {
        let mut stderr = std::io::stderr().lock();
        if self.enable_color {
            let _ = writeln!(
                stderr,
                "[{}] {}{}{}: {}",
                ts,
                level.color(),
                level.as_str(),
                COLOR_RESET,
                msg
            );
        } else {
            let _ = writeln!(stderr, "[{}] {}: {}", ts, level.as_str(), msg);
        }
        let _ = stderr.flush();
    }
}

static GLOBAL_LOGGER: Mutex<LoggerState> = Mutex::new(LoggerState {
    min_level: LogLevel::Info,
    targets: LOG_TARGET_STDERR | LOG_TARGET_FILE,
    log_file: String::new(),
    enable_color: true,
    max_file_size: 10 * 1024 * 1024,
    rotate_count: 5,
    file: None,
    current_size: 0,
});

#[cfg(target_os = "android")]
#[link(name = "log")]
extern "C" {
    fn __android_log_write(prio: i32, tag: *const libc::c_char, text: *const libc::c_char) -> i32;
}

#[cfg(target_os = "android")]
fn write_to_logcat(level: LogLevel, msg: &str) {
    let prio = level.android_prio();
    let tag = b"atpd\0";
    let formatted = match std::ffi::CString::new(format!("[{}] {}", level.as_str(), msg)) {
        Ok(c) => c,
        Err(_) => return,
    };
    // SAFETY: tag and formatted are valid null-terminated C string buffers passed to liblog.
    unsafe {
        __android_log_write(
            prio,
            tag.as_ptr() as *const libc::c_char,
            formatted.as_ptr(),
        );
    }
}

#[cfg(not(target_os = "android"))]
fn write_to_logcat(_level: LogLevel, _msg: &str) {}

fn get_app_dir() -> String {
    if let Ok(exe_path) = std::env::current_exe() {
        if let Some(parent) = exe_path.parent() {
            if parent.file_name().and_then(|s| s.to_str()) == Some("bin") {
                if let Some(grandparent) = parent.parent() {
                    return grandparent.to_string_lossy().into_owned();
                }
            }
            return parent.to_string_lossy().into_owned();
        }
    }
    if std::path::Path::new("/data/adb/atp").exists() {
        return "/data/adb/atp".to_string();
    }
    ".".to_string()
}

pub fn log_set_level(level: LogLevel) {
    let mut state = match GLOBAL_LOGGER.lock() {
        Ok(s) => s,
        Err(p) => p.into_inner(),
    };
    state.min_level = level;
}

pub fn log_set_file<P: AsRef<std::path::Path>>(path: P) {
    let mut state = match GLOBAL_LOGGER.lock() {
        Ok(s) => s,
        Err(p) => p.into_inner(),
    };
    state.log_file = path.as_ref().to_string_lossy().into_owned();
    state.close_file();
    if (state.targets & LOG_TARGET_FILE) != 0 {
        state.open_file();
    }
}

pub fn log_set_color(enable: bool) {
    let mut state = match GLOBAL_LOGGER.lock() {
        Ok(s) => s,
        Err(p) => p.into_inner(),
    };
    state.enable_color = enable;
}

pub fn log_init() {
    crate::utils::time::atp_timezone_init();
    let mut state = match GLOBAL_LOGGER.lock() {
        Ok(s) => s,
        Err(p) => p.into_inner(),
    };
    if state.log_file.is_empty() {
        let app_dir = get_app_dir();
        let run_dir = format!("{}/run", app_dir);
        let _ = std::fs::create_dir_all(&run_dir);
        state.log_file = format!("{}/atp.log", run_dir);
    }
    state.close_file();
    state.open_file();
}

pub fn logger_init() {
    log_init();
}

pub fn log_write(
    level: LogLevel,
    _file: &str,
    _line: u32,
    _func: &str,
    args: std::fmt::Arguments<'_>,
) {
    let min_level = {
        let state = match GLOBAL_LOGGER.lock() {
            Ok(s) => s,
            Err(p) => p.into_inner(),
        };
        state.min_level
    };

    if !level.is_emittable() || level < min_level {
        return;
    }

    let msg = args.to_string();

    write_to_logcat(level, &msg);

    let ts = crate::utils::time::get_timestamp();

    let mut state = match GLOBAL_LOGGER.lock() {
        Ok(s) => s,
        Err(p) => p.into_inner(),
    };
    if (state.targets & LOG_TARGET_FILE) != 0 {
        state.write_to_file(level, &msg, &ts);
    }
    if (state.targets & LOG_TARGET_STDERR) != 0 {
        state.write_to_stderr(level, &msg, &ts);
    }
}

pub fn log(level: LogLevel, file: &str, line: u32, func: &str, args: std::fmt::Arguments<'_>) {
    log_write(level, file, line, func, args);
}

#[macro_export]
macro_rules! log_debug {
    ($($arg:tt)*) => {
        $crate::logger::log(
            $crate::logger::LogLevel::Debug,
            file!(),
            line!(),
            "",
            format_args!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! log_info {
    ($($arg:tt)*) => {
        $crate::logger::log(
            $crate::logger::LogLevel::Info,
            file!(),
            line!(),
            "",
            format_args!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! log_warn {
    ($($arg:tt)*) => {
        $crate::logger::log(
            $crate::logger::LogLevel::Warn,
            file!(),
            line!(),
            "",
            format_args!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! log_error {
    ($($arg:tt)*) => {
        $crate::logger::log(
            $crate::logger::LogLevel::Error,
            file!(),
            line!(),
            "",
            format_args!($($arg)*),
        )
    };
}

#[cfg(test)]
pub(crate) static TEST_LOGGER_MUTEX: Mutex<()> = Mutex::new(());

#[cfg(test)]
mod tests {
    use super::*;

    fn target_is_unchanged(path: &std::path::Path) -> bool {
        let content = std::fs::read(path).unwrap();
        content == b"safe\n"
    }

    #[test]
    fn test_logger_file_safety_ported() {
        let _lock = TEST_LOGGER_MUTEX.lock().unwrap();

        let dir = std::env::temp_dir().join(format!("atpd-log-safety-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();

        let target = dir.join("target");
        let symlink_path = dir.join("symlink");
        let hardlink_path = dir.join("hardlink");

        std::fs::write(&target, b"safe\n").unwrap();

        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(&target, &symlink_path).unwrap();
            std::fs::hard_link(&target, &hardlink_path).unwrap();
        }

        {
            let mut state = GLOBAL_LOGGER.lock().unwrap();
            state.targets = LOG_TARGET_FILE;
        }
        log_set_file(&symlink_path);
        log_info!("must not follow symlink");
        assert!(target_is_unchanged(&target));

        log_set_file(&hardlink_path);
        log_info!("must not follow hardlink");
        {
            let mut state = GLOBAL_LOGGER.lock().unwrap();
            state.close_file();
        }
        assert!(target_is_unchanged(&target));

        // Restore default logger configuration
        {
            let mut state = GLOBAL_LOGGER.lock().unwrap();
            state.targets = LOG_TARGET_STDERR;
            state.log_file = String::new();
            state.close_file();
        }

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn test_logger_rotation() {
        let _lock = TEST_LOGGER_MUTEX.lock().unwrap();

        let dir = std::env::temp_dir().join(format!("atpd-log-rotate-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();

        let log_file = dir.join("test.log");

        {
            let mut state = GLOBAL_LOGGER.lock().unwrap();
            state.max_file_size = 80;
            state.rotate_count = 3;
            state.targets = LOG_TARGET_FILE;
            state.log_file = log_file.to_string_lossy().into_owned();
            state.close_file();
            state.open_file();
        }

        for i in 0..10 {
            log_info!("message number {}", i);
        }

        {
            let mut state = GLOBAL_LOGGER.lock().unwrap();
            state.close_file();
        }

        assert!(log_file.exists());
        assert!(dir.join("test.log.1").exists());

        // Restore defaults
        {
            let mut state = GLOBAL_LOGGER.lock().unwrap();
            state.max_file_size = 10 * 1024 * 1024;
            state.rotate_count = 5;
            state.targets = LOG_TARGET_STDERR;
            state.log_file = String::new();
            state.close_file();
        }

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn test_log_levels_and_macros() {
        let _lock = TEST_LOGGER_MUTEX.lock().unwrap();

        log_set_level(LogLevel::Debug);

        log_debug!("test debug message {}", 1);
        log_info!("test info message {}", 2);
        log_warn!("test warn message {}", 3);
        log_error!("test error message {}", 4);

        // Restore defaults
        log_set_level(LogLevel::Info);
    }
}
