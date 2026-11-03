use std::ffi::{CStr, CString};
use std::io;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceCredentials {
    pub uid: libc::uid_t,
    pub gid: libc::gid_t,
    pub init_groups: bool,
    pub user_name: String,
}

fn parse_uid(text: &str) -> io::Result<libc::uid_t> {
    if text.is_empty() || !text.chars().all(|c| c.is_ascii_digit()) {
        return Err(io::Error::from_raw_os_error(libc::EINVAL));
    }
    match text.parse::<u32>() {
        Ok(v) => Ok(v as libc::uid_t),
        Err(_) => Err(io::Error::from_raw_os_error(libc::EINVAL)),
    }
}

fn parse_gid(text: &str) -> io::Result<libc::gid_t> {
    if text.is_empty() || !text.chars().all(|c| c.is_ascii_digit()) {
        return Err(io::Error::from_raw_os_error(libc::EINVAL));
    }
    match text.parse::<u32>() {
        Ok(v) => Ok(v as libc::gid_t),
        Err(_) => Err(io::Error::from_raw_os_error(libc::EINVAL)),
    }
}

pub fn lookup_passwd_r(name: &str) -> io::Result<Option<(libc::uid_t, libc::gid_t, String)>> {
    let c_name = match CString::new(name) {
        Ok(c) => c,
        Err(_) => return Ok(None),
    };

    let mut pwd = std::mem::MaybeUninit::<libc::passwd>::uninit();
    let mut result: *mut libc::passwd = std::ptr::null_mut();
    let mut buf = vec![0u8; 1024];

    loop {
        // SAFETY: calling getpwnam_r with allocated buffer and pointer to uninit passwd struct.
        let ret = unsafe {
            libc::getpwnam_r(
                c_name.as_ptr(),
                pwd.as_mut_ptr(),
                buf.as_mut_ptr() as *mut libc::c_char,
                buf.len(),
                &mut result,
            )
        };

        if ret == libc::ERANGE {
            buf.resize(buf.len() * 2, 0);
            continue;
        }

        if ret != 0 {
            return Err(io::Error::from_raw_os_error(ret));
        }

        if result.is_null() {
            return Ok(None);
        }

        // SAFETY: result is non-null, meaning getpwnam_r successfully initialized pwd.
        let pwd = unsafe { pwd.assume_init() };
        // SAFETY: pw_name is guaranteed to be a valid null-terminated string inside buffer.
        let user_name = unsafe { CStr::from_ptr(pwd.pw_name) }
            .to_string_lossy()
            .into_owned();

        return Ok(Some((pwd.pw_uid, pwd.pw_gid, user_name)));
    }
}

pub fn lookup_group_r(name: &str) -> io::Result<Option<libc::gid_t>> {
    let c_name = match CString::new(name) {
        Ok(c) => c,
        Err(_) => return Ok(None),
    };

    let mut grp = std::mem::MaybeUninit::<libc::group>::uninit();
    let mut result: *mut libc::group = std::ptr::null_mut();
    let mut buf = vec![0u8; 1024];

    loop {
        // SAFETY: calling getgrnam_r with allocated buffer and pointer to uninit group struct.
        let ret = unsafe {
            libc::getgrnam_r(
                c_name.as_ptr(),
                grp.as_mut_ptr(),
                buf.as_mut_ptr() as *mut libc::c_char,
                buf.len(),
                &mut result,
            )
        };

        if ret == libc::ERANGE {
            buf.resize(buf.len() * 2, 0);
            continue;
        }

        if ret != 0 {
            return Err(io::Error::from_raw_os_error(ret));
        }

        if result.is_null() {
            return Ok(None);
        }

        // SAFETY: result is non-null, meaning getgrnam_r successfully initialized grp.
        let grp = unsafe { grp.assume_init() };

        return Ok(Some(grp.gr_gid));
    }
}

pub fn service_credentials_resolve_with_lookups<P, G>(
    user: Option<&str>,
    group: Option<&str>,
    default_uid: libc::uid_t,
    default_gid: libc::gid_t,
    mut passwd_lookup: P,
    mut group_lookup: G,
) -> io::Result<ServiceCredentials>
where
    P: FnMut(&str) -> Option<(libc::uid_t, libc::gid_t, String)>,
    G: FnMut(&str) -> Option<libc::gid_t>,
{
    let mut credentials = ServiceCredentials {
        uid: default_uid,
        gid: default_gid,
        init_groups: false,
        user_name: String::new(),
    };

    if let Some(user_str) = user {
        if !user_str.is_empty() {
            if user_str == "root" {
                credentials.uid = 0;
                credentials.gid = 0;
            } else if user_str.starts_with(|c: char| c.is_ascii_digit()) {
                credentials.uid = parse_uid(user_str)?;
            } else {
                let pwd = passwd_lookup(user_str)
                    .ok_or_else(|| io::Error::from_raw_os_error(libc::ENOENT))?;
                credentials.uid = pwd.0;
                credentials.gid = pwd.1;
                credentials.init_groups = true;
                if pwd.2.len() >= 64 {
                    return Err(io::Error::from_raw_os_error(libc::ENAMETOOLONG));
                }
                credentials.user_name = pwd.2;
            }
        }
    }

    if let Some(group_str) = group {
        if !group_str.is_empty() {
            if group_str == "root" {
                credentials.gid = 0;
            } else if group_str.starts_with(|c: char| c.is_ascii_digit()) {
                credentials.gid = parse_gid(group_str)?;
            } else {
                let grp_gid = group_lookup(group_str)
                    .ok_or_else(|| io::Error::from_raw_os_error(libc::ENOENT))?;
                credentials.gid = grp_gid;
            }
        }
    }

    Ok(credentials)
}

pub fn service_credentials_resolve(
    user: Option<&str>,
    group: Option<&str>,
    default_uid: libc::uid_t,
    default_gid: libc::gid_t,
) -> io::Result<ServiceCredentials> {
    service_credentials_resolve_with_lookups(
        user,
        group,
        default_uid,
        default_gid,
        |name| lookup_passwd_r(name).ok().flatten(),
        |name| lookup_group_r(name).ok().flatten(),
    )
}
