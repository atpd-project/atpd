//! Timezone detection, tzdata extraction, and timestamp formatting for ATPD.

use std::sync::Mutex;

extern "C" {
    fn tzset();
}

const ANDROID_TZDATA_PATHS: &[&str] = &[
    "/apex/com.android.tzdata/etc/tz/tzdata",
    "/apex/com.android.runtime/etc/tz/tzdata",
    "/system/usr/share/zoneinfo/tzdata",
    "/data/misc/zoneinfo/current/tzdata",
    "/system/etc/tzdata",
    "/vendor/etc/tzdata",
];

const TZ_FALLBACKS: &[(&str, &str)] = &[
    ("Asia/Shanghai", "CST-8"),
    ("Asia/Chongqing", "CST-8"),
    ("Asia/Harbin", "CST-8"),
    ("Asia/Urumqi", "XJT-6"),
    ("Asia/Hong_Kong", "HKT-8"),
    ("Asia/Macau", "CST-8"),
    ("Asia/Taipei", "CST-8"),
    ("Asia/Tokyo", "JST-9"),
    ("Asia/Seoul", "KST-9"),
    ("Asia/Singapore", "SGT-8"),
    ("Asia/Kolkata", "IST-5:30"),
    ("Asia/Calcutta", "IST-5:30"),
    ("Asia/Bangkok", "ICT-7"),
    ("Asia/Jakarta", "WIB-7"),
    ("Asia/Dubai", "GST-4"),
    ("Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"),
    ("Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Moscow", "MSK-3"),
    ("America/New_York", "EST5EDT,M3.2.0,M11.1.0"),
    ("America/Chicago", "CST6CDT,M3.2.0,M11.1.0"),
    ("America/Denver", "MST7MDT,M3.2.0,M11.1.0"),
    ("America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"),
    ("Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"),
    ("UTC", "UTC0"),
    ("GMT", "GMT0"),
];

struct TimezoneState {
    initialized: bool,
    tz_name: String,
    localtime_file: String,
}

static TZ_STATE: Mutex<TimezoneState> = Mutex::new(TimezoneState {
    initialized: false,
    tz_name: String::new(),
    localtime_file: String::new(),
});

fn lookup_posix_fallback(name: &str) -> Option<&'static str> {
    for &(tz_name, posix_tz) in TZ_FALLBACKS {
        if tz_name == name {
            return Some(posix_tz);
        }
    }
    None
}

/// Extract POSIX TZ string from the footer of a TZif v2/v3 buffer.
pub fn extract_posix_tz_from_tzif(buf: &[u8]) -> Option<String> {
    if buf.len() < 8 {
        return None;
    }
    if !buf.starts_with(b"TZif2") && !buf.starts_with(b"TZif3") {
        return None;
    }
    if buf.last() != Some(&b'\n') {
        return None;
    }
    let without_last = &buf[..buf.len() - 1];
    let prev_newline = without_last.iter().rposition(|&b| b == b'\n')?;
    let tz_bytes = &without_last[prev_newline + 1..];
    if tz_bytes.is_empty() {
        return None;
    }
    let s = std::str::from_utf8(tz_bytes).ok()?;
    Some(s.to_string())
}

/// Extract TZif payload for a given timezone name from an Android monolithic tzdata buffer.
pub fn extract_tzif_from_tzdata(tzdata: &[u8], tz_name: &str) -> Option<Vec<u8>> {
    if tzdata.len() < 24 || !tzdata.starts_with(b"tzdata") {
        return None;
    }

    let index_offset = u32::from_be_bytes(tzdata[12..16].try_into().ok()?) as usize;
    let data_offset = u32::from_be_bytes(tzdata[16..20].try_into().ok()?) as usize;

    if data_offset <= index_offset || index_offset < 24 || data_offset > tzdata.len() {
        return None;
    }

    let index_bytes = &tzdata[index_offset..data_offset];
    let entry_size = 52;
    let num_entries = index_bytes.len() / entry_size;

    for i in 0..num_entries {
        let entry_slice = &index_bytes[i * entry_size..(i + 1) * entry_size];
        let name_bytes = &entry_slice[0..40];
        let name_len = name_bytes.iter().position(|&b| b == 0).unwrap_or(40);
        let name = std::str::from_utf8(&name_bytes[..name_len]).ok()?;
        if name == tz_name {
            let start = u32::from_be_bytes(entry_slice[40..44].try_into().ok()?) as usize;
            let len = u32::from_be_bytes(entry_slice[44..48].try_into().ok()?) as usize;
            if !(4..=1024 * 1024).contains(&len) {
                return None;
            }
            let payload_start = data_offset + start;
            let payload_end = payload_start + len;
            if payload_end > tzdata.len() {
                return None;
            }
            let payload = &tzdata[payload_start..payload_end];
            if !payload.starts_with(b"TZif") {
                return None;
            }
            return Some(payload.to_vec());
        }
    }
    None
}

#[cfg(target_os = "android")]
fn get_android_system_property(key: &str) -> Option<String> {
    extern "C" {
        fn __system_property_get(
            name: *const libc::c_char,
            value: *mut libc::c_char,
        ) -> libc::c_int;
    }
    let c_key = std::ffi::CString::new(key).ok()?;
    let mut buf = [0u8; 92];
    let len = {
        // SAFETY: Calling bionic libc __system_property_get with valid key and stack buffer.
        unsafe { __system_property_get(c_key.as_ptr(), buf.as_mut_ptr() as *mut libc::c_char) }
    };
    if len > 0 {
        let val = std::str::from_utf8(&buf[..len as usize])
            .ok()?
            .trim()
            .to_string();
        if !val.is_empty() {
            return Some(val);
        }
    }
    None
}

#[cfg(not(target_os = "android"))]
fn get_android_system_property(_key: &str) -> Option<String> {
    None
}

fn get_android_prop(key: &str) -> Option<String> {
    if let Some(val) = get_android_system_property(key) {
        return Some(val);
    }
    let output = std::process::Command::new("/system/bin/getprop")
        .arg(key)
        .output()
        .or_else(|_| std::process::Command::new("getprop").arg(key).output())
        .ok()?;

    if output.status.success() {
        let val = String::from_utf8_lossy(&output.stdout).trim().to_string();
        if !val.is_empty() {
            return Some(val);
        }
    }
    None
}

fn detect_device_timezone() -> Option<String> {
    if let Some(tz) = get_android_prop("persist.sys.timezone") {
        return Some(tz);
    }
    if let Some(tz) = get_android_prop("persist.sys.timezone.default") {
        return Some(tz);
    }
    if let Some(locale) = get_android_prop("ro.product.locale") {
        let lower = locale.to_lowercase();
        if lower.starts_with("zh") {
            return Some("Asia/Shanghai".to_string());
        } else if lower.starts_with("ja") {
            return Some("Asia/Tokyo".to_string());
        } else if lower.starts_with("ko") {
            return Some("Asia/Seoul".to_string());
        }
    }
    None
}

fn derive_posix_tz_from_date() -> Option<String> {
    let output = std::process::Command::new("/system/bin/date")
        .arg("+%z")
        .output()
        .or_else(|_| std::process::Command::new("date").arg("+%z").output())
        .ok()?;

    if !output.status.success() {
        return None;
    }

    let out = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if out.len() != 5 {
        return None;
    }
    let bytes = out.as_bytes();
    let sign = bytes[0] as char;
    if sign != '+' && sign != '-' {
        return None;
    }

    let h1 = (bytes[1] as char).to_digit(10)? as i32;
    let h2 = (bytes[2] as char).to_digit(10)? as i32;
    let m1 = (bytes[3] as char).to_digit(10)? as i32;
    let m2 = (bytes[4] as char).to_digit(10)? as i32;

    let hours = h1 * 10 + h2;
    let mins = m1 * 10 + m2;

    let posix_sign = if sign == '+' { '-' } else { '+' };
    if mins == 0 {
        Some(format!("UTC{}{}", posix_sign, hours))
    } else {
        Some(format!("UTC{}{}:{:02}", posix_sign, hours, mins))
    }
}

fn extract_tzif_from_tzdata_file(
    tzdata_path: &str,
    tz_name: &str,
    out_file: &str,
) -> Option<String> {
    let data = std::fs::read(tzdata_path).ok()?;
    let payload = extract_tzif_from_tzdata(&data, tz_name)?;
    let posix_tz = extract_posix_tz_from_tzif(&payload);

    let tmp_file = format!("{}.tmp.{}", out_file, std::process::id());
    if std::fs::write(&tmp_file, &payload).is_ok() {
        if std::fs::rename(&tmp_file, out_file).is_ok() {
            return posix_tz;
        }
        let _ = std::fs::remove_file(&tmp_file);
    }
    posix_tz
}

fn extract_android_tzdata(tz_name: &str, out_file: &str) -> Option<String> {
    for &path in ANDROID_TZDATA_PATHS {
        if std::path::Path::new(path).is_file() {
            if let Some(posix_tz) = extract_tzif_from_tzdata_file(path, tz_name, out_file) {
                return Some(posix_tz);
            }
        }
    }
    None
}

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

fn resolve_localtime_target_path() -> String {
    let app_dir = get_app_dir();
    let run_dir = std::path::Path::new(&app_dir).join("run");
    if std::fs::create_dir_all(&run_dir).is_ok() {
        let candidate = run_dir.join("localtime");
        return candidate.to_string_lossy().into_owned();
    }
    let data_local_tmp = std::path::Path::new("/data/local/tmp");
    if data_local_tmp.is_dir() {
        return "/data/local/tmp/atpd_localtime".to_string();
    }
    std::env::temp_dir()
        .join("atpd_localtime")
        .to_string_lossy()
        .into_owned()
}

fn apply_tz(tz_val: &str) {
    std::env::set_var("TZ", tz_val);
    // SAFETY: tzset updates internal libc timezone state based on current TZ env variable.
    unsafe { tzset() };
}

/// The first call must run before any thread is spawned: setting TZ races with libc getenv in
/// other threads. Later calls (config reload) find TZ already set and only re-run tzset.
pub fn atp_timezone_init() -> i32 {
    let mut state = match TZ_STATE.lock() {
        Ok(s) => s,
        Err(p) => p.into_inner(),
    };

    // 1. If TZ environment is explicitly set to a valid non-empty value, respect it
    if let Ok(env_tz) = std::env::var("TZ") {
        if !env_tz.is_empty()
            && (env_tz.starts_with('/')
                || env_tz.starts_with(':')
                || env_tz.contains('-')
                || env_tz.contains('+')
                || env_tz.contains('0'))
        {
            // SAFETY: tzset initializes time conversion information from current TZ env.
            unsafe { tzset() };
            state.tz_name = env_tz;
            state.initialized = true;
            return 0;
        }
    }

    // 2. Check if standard Linux /etc/localtime exists and is readable
    if std::path::Path::new("/etc/localtime").exists() {
        // SAFETY: tzset initializes time conversion information from /etc/localtime.
        unsafe { tzset() };
        if state.tz_name.is_empty() {
            state.tz_name = "Localtime".to_string();
        }
        state.initialized = true;
        return 0;
    }

    // 3. Detect Android Device Timezone
    let detected_tz = match detect_device_timezone() {
        Some(tz) => tz,
        None => {
            if let Some(date_posix_tz) = derive_posix_tz_from_date() {
                apply_tz(&date_posix_tz);
                state.tz_name = date_posix_tz;
                state.initialized = true;
                return 0;
            }
            "Asia/Shanghai".to_string()
        }
    };

    state.tz_name = detected_tz.clone();

    // 4. Prepare target localtime file and try extracting Android tzdata
    if state.localtime_file.is_empty() {
        state.localtime_file = resolve_localtime_target_path();
    }

    if let Some(extracted_posix_tz) = extract_android_tzdata(&detected_tz, &state.localtime_file) {
        if std::path::Path::new(&state.localtime_file).exists() {
            apply_tz(&state.localtime_file);
            state.initialized = true;
            return 0;
        }
        if !extracted_posix_tz.is_empty() {
            apply_tz(&extracted_posix_tz);
            state.initialized = true;
            return 0;
        }
    }

    // 5. Fallback: posix lookup table
    if let Some(posix_tz) = lookup_posix_fallback(&detected_tz) {
        apply_tz(posix_tz);
        state.initialized = true;
        return 0;
    }

    // 6. Ultimate fallback: derive from system date or CST-8
    if let Some(date_posix_tz) = derive_posix_tz_from_date() {
        apply_tz(&date_posix_tz);
        state.initialized = true;
        return 0;
    }

    apply_tz("CST-8");
    state.initialized = true;
    0
}

pub fn atp_timezone_get_name(buf: &mut [u8]) -> i32 {
    if buf.is_empty() {
        return -1;
    }
    let name = timezone_name();
    let bytes = name.as_bytes();
    let copy_len = bytes.len().min(buf.len().saturating_sub(1));
    buf[..copy_len].copy_from_slice(&bytes[..copy_len]);
    buf[copy_len] = 0;
    0
}

pub fn timezone_name() -> String {
    let mut state = match TZ_STATE.lock() {
        Ok(s) => s,
        Err(p) => p.into_inner(),
    };
    if !state.initialized {
        drop(state);
        atp_timezone_init();
        state = match TZ_STATE.lock() {
            Ok(s) => s,
            Err(p) => p.into_inner(),
        };
    }
    if state.tz_name.is_empty() {
        "UTC".to_string()
    } else {
        state.tz_name.clone()
    }
}

pub fn atp_timezone_get_offset_sec() -> i64 {
    {
        let state = match TZ_STATE.lock() {
            Ok(s) => s,
            Err(p) => p.into_inner(),
        };
        if !state.initialized {
            drop(state);
            atp_timezone_init();
        }
    }

    // SAFETY: time() returns current time_t when passed null pointer.
    let now = unsafe { libc::time(std::ptr::null_mut()) };
    // SAFETY: zeroed tm struct for localtime_r.
    let mut tm_local: libc::tm = unsafe { std::mem::zeroed() };
    // SAFETY: localtime_r with valid now and tm pointers.
    let ret = unsafe { libc::localtime_r(&now, &mut tm_local) };
    if ret.is_null() {
        return 0;
    }

    #[cfg(any(target_os = "android", target_os = "linux"))]
    {
        tm_local.tm_gmtoff as i64
    }

    #[cfg(not(any(target_os = "android", target_os = "linux")))]
    {
        // SAFETY: zeroed tm struct for gmtime_r.
        let mut tm_utc: libc::tm = unsafe { std::mem::zeroed() };
        // SAFETY: gmtime_r with valid now and tm pointers.
        let ret_utc = unsafe { libc::gmtime_r(&now, &mut tm_utc) };
        if ret_utc.is_null() {
            return 0;
        }
        tm_utc.tm_isdst = tm_local.tm_isdst;
        // SAFETY: mktime with valid pointers to tm structs.
        unsafe { (libc::mktime(&mut tm_local) - libc::mktime(&mut tm_utc)) as i64 }
    }
}

pub fn get_timestamp() -> String {
    // SAFETY: time() with null pointer retrieves current seconds.
    let now = unsafe { libc::time(std::ptr::null_mut()) };
    format_timestamp(now)
}

pub fn format_timestamp(now: libc::time_t) -> String {
    // SAFETY: zeroed tm struct for localtime_r.
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    // SAFETY: localtime_r with valid now and tm pointers.
    let ret = unsafe { libc::localtime_r(&now, &mut tm) };
    if ret.is_null() {
        "0000-00-00 00:00:00".to_string()
    } else {
        format!(
            "{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec
        )
    }
}

#[cfg(test)]
pub(crate) static TEST_TZ_MUTEX: Mutex<()> = Mutex::new(());

#[cfg(test)]
mod tests {
    use super::*;

    fn create_mock_android_tzdata(path: &std::path::Path) {
        let tzif_len = 0x100;
        let mut tzif_buf = vec![0u8; tzif_len];
        tzif_buf[..5].copy_from_slice(b"TZif2");
        let posix_footer = b"\nCST-8\n";
        tzif_buf[tzif_len - posix_footer.len()..].copy_from_slice(posix_footer);

        let mut file_buf = Vec::new();
        let mut magic = [0u8; 12];
        magic[..11].copy_from_slice(b"tzdata2024a");
        file_buf.extend_from_slice(&magic);
        file_buf.extend_from_slice(&24u32.to_be_bytes()); // index_offset
        file_buf.extend_from_slice(&(24 + 52 * 2u32).to_be_bytes()); // data_offset
        file_buf.extend_from_slice(&(24 + 52 * 2 + (tzif_len * 2) as u32).to_be_bytes()); // zonetab_offset

        // Entry 1: Asia/Shanghai
        let mut name1 = [0u8; 40];
        let n1_bytes = b"Asia/Shanghai";
        name1[..n1_bytes.len()].copy_from_slice(n1_bytes);
        file_buf.extend_from_slice(&name1);
        file_buf.extend_from_slice(&0u32.to_be_bytes()); // start
        file_buf.extend_from_slice(&(tzif_len as u32).to_be_bytes()); // len
        file_buf.extend_from_slice(&0u32.to_be_bytes()); // unused

        // Entry 2: Asia/Tokyo
        let mut name2 = [0u8; 40];
        let n2_bytes = b"Asia/Tokyo";
        name2[..n2_bytes.len()].copy_from_slice(n2_bytes);
        file_buf.extend_from_slice(&name2);
        file_buf.extend_from_slice(&(tzif_len as u32).to_be_bytes()); // start
        file_buf.extend_from_slice(&(tzif_len as u32).to_be_bytes()); // len
        file_buf.extend_from_slice(&0u32.to_be_bytes()); // unused

        // Data block 1 (Shanghai)
        file_buf.extend_from_slice(&tzif_buf);

        // Data block 2 (Tokyo)
        let tokyo_footer = b"\nJST-9\n";
        tzif_buf[tzif_len - tokyo_footer.len()..].copy_from_slice(tokyo_footer);
        file_buf.extend_from_slice(&tzif_buf);

        std::fs::write(path, &file_buf).unwrap();
    }

    #[test]
    fn test_synthetic_tzdata_and_posix_fallback() {
        let _lock = TEST_TZ_MUTEX.lock().unwrap();

        let run_dir = std::env::temp_dir().join(format!("atp_tz_test_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&run_dir);
        std::fs::create_dir_all(&run_dir).unwrap();

        let mock_tzdata = run_dir.join("tzdata");
        create_mock_android_tzdata(&mock_tzdata);

        let tzdata_bytes = std::fs::read(&mock_tzdata).unwrap();
        let shanghai_payload = extract_tzif_from_tzdata(&tzdata_bytes, "Asia/Shanghai").unwrap();
        assert_eq!(
            extract_posix_tz_from_tzif(&shanghai_payload),
            Some("CST-8".to_string())
        );

        let tokyo_payload = extract_tzif_from_tzdata(&tzdata_bytes, "Asia/Tokyo").unwrap();
        assert_eq!(
            extract_posix_tz_from_tzif(&tokyo_payload),
            Some("JST-9".to_string())
        );

        // Test fallback POSIX mapping exactly as test_timezone.c
        std::env::set_var("TZ", "CST-8");
        // SAFETY: updating libc timezone state with test TZ.
        unsafe { tzset() };

        let now: libc::time_t = 1700000000; // 2023-11-14 22:13:20 UTC
                                            // SAFETY: zeroed tm struct for test localtime_r.
        let mut tm_res: libc::tm = unsafe { std::mem::zeroed() };
        // SAFETY: localtime_r with valid now and tm pointers.
        unsafe { libc::localtime_r(&now, &mut tm_res) };

        assert_eq!(tm_res.tm_hour, 6);
        assert_eq!(tm_res.tm_min, 13);
        assert_eq!(tm_res.tm_sec, 20);
        assert_eq!(tm_res.tm_mday, 15);

        let rc = atp_timezone_init();
        assert_eq!(rc, 0);

        let mut tz_name_buf = [0u8; 64];
        let rc_name = atp_timezone_get_name(&mut tz_name_buf);
        assert_eq!(rc_name, 0);
        let name_str = timezone_name();
        assert!(!name_str.is_empty());

        let offset = atp_timezone_get_offset_sec();
        assert_eq!(offset, 28800); // 8 hours in seconds

        let _ = std::fs::remove_dir_all(&run_dir);
    }

    #[test]
    fn test_localtime_formatting() {
        let _lock = TEST_TZ_MUTEX.lock().unwrap();

        let now: libc::time_t = 1700000000;

        // CST-8
        std::env::set_var("TZ", "CST-8");
        // SAFETY: updating libc timezone state with test TZ.
        unsafe { tzset() };
        assert_eq!(format_timestamp(now), "2023-11-15 06:13:20");

        // JST-9
        std::env::set_var("TZ", "JST-9");
        // SAFETY: updating libc timezone state with test TZ.
        unsafe { tzset() };
        assert_eq!(format_timestamp(now), "2023-11-15 07:13:20");

        // EST5
        std::env::set_var("TZ", "EST5");
        // SAFETY: updating libc timezone state with test TZ.
        unsafe { tzset() };
        assert_eq!(format_timestamp(now), "2023-11-14 17:13:20");
    }
}
