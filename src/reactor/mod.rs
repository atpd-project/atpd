pub mod timer;

#[cfg(test)]
mod tests;

use timer::TimerEntry;
pub use timer::TimerId;

use std::collections::HashMap;
use std::io;
use std::os::unix::io::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::sync::Arc;

pub const READ: u32 = libc::EPOLLIN as u32;
pub const WRITE: u32 = libc::EPOLLOUT as u32;
pub const ERROR: u32 = libc::EPOLLERR as u32;
pub const HANGUP: u32 = libc::EPOLLHUP as u32;
pub const EDGE: u32 = libc::EPOLLET as u32;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Event {
    Io { token: u64, events: u32 },
    Timer(TimerId),
    Signal(i32),
    Wakeup,
}
// TimerIds are never reused: a timer cancelled after its event was queued in the same wait()
// batch still appears here, so callers must match the id against their stored Option<TimerId>.

// Shares ownership of the eventfd so a Waker outliving the Reactor never writes to a reused fd number.
#[derive(Debug, Clone)]
pub struct Waker {
    fd: Arc<OwnedFd>,
}

impl Waker {
    pub fn wake(&self) -> io::Result<()> {
        let val: u64 = 1;
        loop {
            // SAFETY: writing 8-byte uint64 value to a valid eventfd descriptor.
            let res = unsafe {
                libc::write(
                    self.fd.as_raw_fd(),
                    &val as *const u64 as *const libc::c_void,
                    std::mem::size_of::<u64>(),
                )
            };
            if res >= 0 {
                return Ok(());
            }
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            if err.raw_os_error() == Some(libc::EAGAIN) {
                return Ok(());
            }
            return Err(err);
        }
    }
}

pub fn now_ms() -> u64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: ts is a valid pointer to timespec.
    unsafe {
        libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts);
    }
    (ts.tv_sec as u64) * 1000 + (ts.tv_nsec as u64) / 1_000_000
}

pub struct Reactor {
    epoll_fd: RawFd,
    event_fd: RawFd,
    event_fd_owner: Arc<OwnedFd>,
    signal_fd: RawFd,
    watched_signals: libc::sigset_t,
    orig_sigmask: libc::sigset_t,
    next_timer_id: u64,
    // shortcut: linear timer scan, fine for < ~32 timers; switch to a heap if timer count grows
    timers: Vec<TimerEntry>,
    fd_tokens: HashMap<RawFd, u64>,
}

impl Reactor {
    pub fn now_ms() -> u64 {
        now_ms()
    }

    pub fn new() -> io::Result<Reactor> {
        // SAFETY: epoll_create1 creates an epoll descriptor with close-on-exec.
        let epoll_fd = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
        if epoll_fd < 0 {
            return Err(io::Error::last_os_error());
        }

        // SAFETY: eventfd creates a counter event descriptor.
        let event_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
        if event_fd < 0 {
            let err = io::Error::last_os_error();
            // SAFETY: cleanup epoll descriptor on failure.
            unsafe { libc::close(epoll_fd) };
            return Err(err);
        }

        let mut watched_signals = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
        // SAFETY: initializing empty sigset for watched signals.
        unsafe { libc::sigemptyset(watched_signals.as_mut_ptr()) };
        // SAFETY: assuming init for sigset after sigemptyset.
        let watched_signals = unsafe { watched_signals.assume_init() };

        let signal_fd =
            // SAFETY: signalfd creates a signal descriptor for empty mask.
            unsafe { libc::signalfd(-1, &watched_signals, libc::SFD_NONBLOCK | libc::SFD_CLOEXEC) };
        if signal_fd < 0 {
            let err = io::Error::last_os_error();
            // SAFETY: cleanup eventfd and epoll descriptors on failure.
            unsafe {
                libc::close(event_fd);
                libc::close(epoll_fd);
            }
            return Err(err);
        }

        let mut orig_sigmask = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
        // SAFETY: reading current thread signal mask without modifying it.
        let ret = unsafe { libc::sigprocmask(0, std::ptr::null(), orig_sigmask.as_mut_ptr()) };
        if ret < 0 {
            let err = io::Error::last_os_error();
            // SAFETY: cleanup open descriptors on failure.
            unsafe {
                libc::close(signal_fd);
                libc::close(event_fd);
                libc::close(epoll_fd);
            }
            return Err(err);
        }
        // SAFETY: assuming init for sigset after sigprocmask.
        let orig_sigmask = unsafe { orig_sigmask.assume_init() };

        let mut event_ev = libc::epoll_event {
            events: libc::EPOLLIN as u32,
            u64: event_fd as u64,
        };
        let ret =
            // SAFETY: registering event_fd in epoll set.
            unsafe { libc::epoll_ctl(epoll_fd, libc::EPOLL_CTL_ADD, event_fd, &mut event_ev) };
        if ret < 0 {
            let err = io::Error::last_os_error();
            // SAFETY: cleanup open descriptors on failure.
            unsafe {
                libc::close(signal_fd);
                libc::close(event_fd);
                libc::close(epoll_fd);
            }
            return Err(err);
        }

        let mut signal_ev = libc::epoll_event {
            events: libc::EPOLLIN as u32,
            u64: signal_fd as u64,
        };
        let ret =
            // SAFETY: registering signal_fd in epoll set.
            unsafe { libc::epoll_ctl(epoll_fd, libc::EPOLL_CTL_ADD, signal_fd, &mut signal_ev) };
        if ret < 0 {
            let err = io::Error::last_os_error();
            // SAFETY: cleanup open descriptors on failure.
            unsafe {
                libc::close(signal_fd);
                libc::close(event_fd);
                libc::close(epoll_fd);
            }
            return Err(err);
        }

        Ok(Reactor {
            epoll_fd,
            event_fd,
            // SAFETY: event_fd is a freshly created, open descriptor owned by nobody else.
            event_fd_owner: Arc::new(unsafe { OwnedFd::from_raw_fd(event_fd) }),
            signal_fd,
            watched_signals,
            orig_sigmask,
            next_timer_id: 0,
            timers: Vec::new(),
            fd_tokens: HashMap::new(),
        })
    }

    pub fn add_fd(&mut self, fd: RawFd, events: u32, token: u64) -> io::Result<()> {
        if fd < 0 {
            return Err(io::Error::from_raw_os_error(libc::EBADF));
        }
        let existed = self.fd_tokens.contains_key(&fd);
        let op = if existed {
            libc::EPOLL_CTL_MOD
        } else {
            libc::EPOLL_CTL_ADD
        };
        let mut ev = libc::epoll_event {
            events,
            u64: fd as u64,
        };
        // SAFETY: epoll_ctl register or modify fd in epoll.
        let ret = unsafe { libc::epoll_ctl(self.epoll_fd, op, fd, &mut ev) };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }
        self.fd_tokens.insert(fd, token);
        Ok(())
    }

    pub fn modify_fd(&mut self, fd: RawFd, events: u32) -> io::Result<()> {
        if !self.fd_tokens.contains_key(&fd) {
            return Err(io::Error::new(io::ErrorKind::NotFound, "fd not registered"));
        }
        let mut ev = libc::epoll_event {
            events,
            u64: fd as u64,
        };
        // SAFETY: epoll_ctl modify event mask for fd.
        let ret = unsafe { libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_MOD, fd, &mut ev) };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    pub fn remove_fd(&mut self, fd: RawFd) -> io::Result<()> {
        if self.fd_tokens.remove(&fd).is_some() {
            // SAFETY: epoll_ctl delete fd from epoll set.
            let ret = unsafe {
                libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_DEL, fd, std::ptr::null_mut())
            };
            if ret < 0 {
                let err = io::Error::last_os_error();
                if err.raw_os_error() != Some(libc::ENOENT)
                    && err.raw_os_error() != Some(libc::EBADF)
                {
                    return Err(err);
                }
            }
        }
        Ok(())
    }

    pub fn add_timer(&mut self, timeout_ms: u64, interval_ms: u64) -> TimerId {
        self.next_timer_id += 1;
        let id = TimerId(self.next_timer_id);
        let expires_ms = Self::now_ms().saturating_add(timeout_ms);
        // shortcut: linear timer scan, fine for < ~32 timers; switch to a heap if timer count grows
        self.timers.push(TimerEntry {
            id,
            expires_ms,
            interval_ms,
        });
        id
    }

    pub fn cancel_timer(&mut self, id: TimerId) -> bool {
        // shortcut: linear timer scan, fine for < ~32 timers; switch to a heap if timer count grows
        if let Some(pos) = self.timers.iter().position(|t| t.id == id) {
            self.timers.remove(pos);
            true
        } else {
            false
        }
    }

    pub fn watch_signal(&mut self, signo: i32) -> io::Result<()> {
        if signo <= 0 {
            return Err(io::Error::from_raw_os_error(libc::EINVAL));
        }

        // SAFETY: adding signal to watched sigset.
        let ret = unsafe { libc::sigaddset(&mut self.watched_signals, signo) };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }

        // SAFETY: updating signalfd descriptor with new mask.
        let sfd = unsafe {
            libc::signalfd(
                self.signal_fd,
                &self.watched_signals,
                libc::SFD_NONBLOCK | libc::SFD_CLOEXEC,
            )
        };
        if sfd < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut block_mask = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
        // SAFETY: initializing block_mask sigset.
        unsafe { libc::sigemptyset(block_mask.as_mut_ptr()) };
        // SAFETY: assuming init for sigset after sigemptyset.
        let mut block_mask = unsafe { block_mask.assume_init() };
        // SAFETY: adding signo to block_mask.
        unsafe { libc::sigaddset(&mut block_mask, signo) };
        // SAFETY: blocking the watched signal via sigprocmask so it is delivered to signalfd.
        let ret = unsafe { libc::sigprocmask(libc::SIG_BLOCK, &block_mask, std::ptr::null_mut()) };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }

        Ok(())
    }

    pub fn waker(&self) -> Waker {
        Waker {
            fd: Arc::clone(&self.event_fd_owner),
        }
    }

    pub fn wait(&mut self, out: &mut Vec<Event>) -> io::Result<()> {
        let initial_len = out.len();
        while out.len() == initial_len {
            let now = Self::now_ms();
            // shortcut: linear timer scan, fine for < ~32 timers; switch to a heap if timer count grows
            let next_deadline = self.timers.iter().map(|t| t.expires_ms).min();
            let timeout_ms = match next_deadline {
                None => -1,
                Some(deadline) => {
                    if deadline <= now {
                        0
                    } else {
                        let diff = deadline - now;
                        if diff > i32::MAX as u64 {
                            i32::MAX
                        } else {
                            diff as i32
                        }
                    }
                }
            };

            let mut epoll_events = [libc::epoll_event { events: 0, u64: 0 }; 64];
            // SAFETY: epoll_wait waits for events on epoll_fd.
            let nfds = unsafe {
                libc::epoll_wait(
                    self.epoll_fd,
                    epoll_events.as_mut_ptr(),
                    epoll_events.len() as libc::c_int,
                    timeout_ms,
                )
            };

            if nfds < 0 {
                let err = io::Error::last_os_error();
                if err.raw_os_error() == Some(libc::EINTR) {
                    self.process_timers(Self::now_ms(), out);
                    continue;
                }
                return Err(err);
            }

            for ev in epoll_events.iter().take(nfds as usize) {
                let fd = ev.u64 as RawFd;
                let events = ev.events;

                if fd == self.event_fd {
                    let mut val = 0u64;
                    loop {
                        // SAFETY: reading uint64 counter to drain eventfd.
                        let res = unsafe {
                            libc::read(
                                self.event_fd,
                                &mut val as *mut u64 as *mut libc::c_void,
                                std::mem::size_of::<u64>(),
                            )
                        };
                        if res < 0 {
                            let err = io::Error::last_os_error();
                            if err.raw_os_error() == Some(libc::EINTR) {
                                continue;
                            }
                            break;
                        }
                        if res == std::mem::size_of::<u64>() as isize {
                            continue;
                        }
                        break;
                    }
                    out.push(Event::Wakeup);
                } else if self.signal_fd >= 0 && fd == self.signal_fd {
                    loop {
                        let mut siginfo = std::mem::MaybeUninit::<libc::signalfd_siginfo>::uninit();
                        // SAFETY: reading signalfd_siginfo structure to drain signalfd.
                        let res = unsafe {
                            libc::read(
                                self.signal_fd,
                                siginfo.as_mut_ptr() as *mut libc::c_void,
                                std::mem::size_of::<libc::signalfd_siginfo>(),
                            )
                        };
                        if res < 0 {
                            let err = io::Error::last_os_error();
                            if err.raw_os_error() == Some(libc::EINTR) {
                                continue;
                            }
                            break;
                        }
                        if res as usize == std::mem::size_of::<libc::signalfd_siginfo>() {
                            // SAFETY: full struct successfully read from signalfd.
                            let siginfo = unsafe { siginfo.assume_init() };
                            out.push(Event::Signal(siginfo.ssi_signo as i32));
                        } else {
                            break;
                        }
                    }
                } else if let Some(&token) = self.fd_tokens.get(&fd) {
                    out.push(Event::Io { token, events });
                }
            }

            self.process_timers(Self::now_ms(), out);
        }

        Ok(())
    }

    fn process_timers(&mut self, now: u64, out: &mut Vec<Event>) {
        // shortcut: linear timer scan, fine for < ~32 timers; switch to a heap if timer count grows
        let mut due_indices: Vec<usize> = Vec::new();
        for (i, t) in self.timers.iter().enumerate() {
            if t.expires_ms <= now {
                due_indices.push(i);
            }
        }

        if due_indices.is_empty() {
            return;
        }

        due_indices.sort_by(|&a, &b| {
            let ta = &self.timers[a];
            let tb = &self.timers[b];
            ta.expires_ms
                .cmp(&tb.expires_ms)
                .then_with(|| ta.id.cmp(&tb.id))
        });

        for &idx in &due_indices {
            out.push(Event::Timer(self.timers[idx].id));
        }

        let mut retained = Vec::with_capacity(self.timers.len());
        for mut t in self.timers.drain(..) {
            if t.expires_ms <= now {
                if t.interval_ms > 0 {
                    t.expires_ms += t.interval_ms;
                    while t.expires_ms <= now {
                        t.expires_ms += t.interval_ms;
                    }
                    retained.push(t);
                }
            } else {
                retained.push(t);
            }
        }
        self.timers = retained;
    }
}

impl Drop for Reactor {
    fn drop(&mut self) {
        // SAFETY: closing epoll descriptor.
        unsafe {
            libc::close(self.epoll_fd);
        }
        if self.signal_fd >= 0 {
            // SAFETY: closing signalfd descriptor.
            unsafe {
                libc::close(self.signal_fd);
            }
        }
        // SAFETY: restoring original process signal mask saved during initialization.
        unsafe {
            libc::sigprocmask(libc::SIG_SETMASK, &self.orig_sigmask, std::ptr::null_mut());
        }
    }
}
