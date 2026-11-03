pub mod timer;

#[cfg(test)]
mod tests;

use timer::TimerEntry;
pub use timer::TimerId;

use rustix::event::epoll;
use rustix::io::Errno;
use std::collections::HashMap;
use std::io;
use std::os::unix::io::{AsFd, AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};
use std::sync::Arc;

pub const READ: u32 = libc::EPOLLIN as u32;
pub const WRITE: u32 = libc::EPOLLOUT as u32;
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
        loop {
            match rustix::io::write(&*self.fd, &1u64.to_ne_bytes()) {
                Ok(_) | Err(Errno::AGAIN) => return Ok(()),
                Err(Errno::INTR) => continue,
                Err(e) => return Err(e.into()),
            }
        }
    }
}

pub fn now_ms() -> u64 {
    let ts = rustix::time::clock_gettime(rustix::time::ClockId::Monotonic);
    (ts.tv_sec as u64) * 1000 + (ts.tv_nsec as u64) / 1_000_000
}

/// Counters reported by the IPC `stats` command, counted as reactor.c does.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct ReactorStats {
    pub events_processed: u64,
    pub timers_fired: u64,
    pub signals_received: u64,
}

pub struct Reactor {
    stats: ReactorStats,
    epoll: OwnedFd,
    event_fd: Arc<OwnedFd>,
    signal_fd: OwnedFd,
    watched_signals: libc::sigset_t,
    orig_sigmask: libc::sigset_t,
    next_timer_id: u64,
    // shortcut: linear timer scan, fine for < ~32 timers; switch to a heap if timer count grows
    timers: Vec<TimerEntry>,
    fd_tokens: HashMap<RawFd, u64>,
}

fn empty_sigset() -> libc::sigset_t {
    let mut set = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
    // SAFETY: sigemptyset fully initializes the set it is given.
    unsafe {
        libc::sigemptyset(set.as_mut_ptr());
        set.assume_init()
    }
}

impl Reactor {
    pub fn now_ms() -> u64 {
        now_ms()
    }

    pub fn new() -> io::Result<Reactor> {
        let epoll = epoll::create(epoll::CreateFlags::CLOEXEC)?;
        let event_fd = rustix::event::eventfd(
            0,
            rustix::event::EventfdFlags::NONBLOCK | rustix::event::EventfdFlags::CLOEXEC,
        )?;

        let watched_signals = empty_sigset();
        // SAFETY: signalfd creates a new descriptor (fd -1) for a valid, empty mask.
        let raw =
            unsafe { libc::signalfd(-1, &watched_signals, libc::SFD_NONBLOCK | libc::SFD_CLOEXEC) };
        if raw < 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: raw is a freshly created descriptor owned by nobody else.
        let signal_fd = unsafe { OwnedFd::from_raw_fd(raw) };

        let mut orig_sigmask = empty_sigset();
        // SAFETY: reading the current signal mask into a valid sigset without modifying it.
        if unsafe { libc::sigprocmask(0, std::ptr::null(), &mut orig_sigmask) } < 0 {
            return Err(io::Error::last_os_error());
        }

        for fd in [event_fd.as_fd(), signal_fd.as_fd()] {
            epoll::add(
                &epoll,
                fd,
                epoll::EventData::new_u64(fd.as_raw_fd() as u64),
                epoll::EventFlags::IN,
            )?;
        }

        Ok(Reactor {
            epoll,
            event_fd: Arc::new(event_fd),
            signal_fd,
            watched_signals,
            orig_sigmask,
            next_timer_id: 0,
            timers: Vec::new(),
            fd_tokens: HashMap::new(),
            stats: ReactorStats::default(),
        })
    }

    pub fn add_fd(&mut self, fd: RawFd, events: u32, token: u64) -> io::Result<()> {
        if fd < 0 {
            return Err(io::Error::from_raw_os_error(libc::EBADF));
        }
        let data = epoll::EventData::new_u64(fd as u64);
        let flags = epoll::EventFlags::from_bits_retain(events);
        // SAFETY: the caller passes an open fd.
        let source = unsafe { BorrowedFd::borrow_raw(fd) };
        if self.fd_tokens.contains_key(&fd) {
            epoll::modify(&self.epoll, source, data, flags)?;
        } else {
            epoll::add(&self.epoll, source, data, flags)?;
        }
        self.fd_tokens.insert(fd, token);
        Ok(())
    }

    pub fn modify_fd(&mut self, fd: RawFd, events: u32) -> io::Result<()> {
        if !self.fd_tokens.contains_key(&fd) {
            return Err(io::Error::new(io::ErrorKind::NotFound, "fd not registered"));
        }
        // SAFETY: fd is registered, so the caller still holds it open.
        let source = unsafe { BorrowedFd::borrow_raw(fd) };
        epoll::modify(
            &self.epoll,
            source,
            epoll::EventData::new_u64(fd as u64),
            epoll::EventFlags::from_bits_retain(events),
        )?;
        Ok(())
    }

    pub fn remove_fd(&mut self, fd: RawFd) -> io::Result<()> {
        if self.fd_tokens.remove(&fd).is_some() {
            // SAFETY: the kernel only uses the number; a closed fd yields EBADF, handled below.
            let source = unsafe { BorrowedFd::borrow_raw(fd) };
            match epoll::delete(&self.epoll, source) {
                Ok(()) | Err(Errno::NOENT) | Err(Errno::BADF) => {}
                Err(e) => return Err(e.into()),
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

        // SAFETY: updating our own signalfd with a valid mask.
        let sfd = unsafe {
            libc::signalfd(
                self.signal_fd.as_raw_fd(),
                &self.watched_signals,
                libc::SFD_NONBLOCK | libc::SFD_CLOEXEC,
            )
        };
        if sfd < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut block_mask = empty_sigset();
        // SAFETY: block_mask is initialized; blocking routes the signal to signalfd.
        let ret = unsafe {
            libc::sigaddset(&mut block_mask, signo);
            libc::sigprocmask(libc::SIG_BLOCK, &block_mask, std::ptr::null_mut())
        };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }

        Ok(())
    }

    pub fn stats(&self) -> ReactorStats {
        self.stats
    }

    pub fn waker(&self) -> Waker {
        Waker {
            fd: Arc::clone(&self.event_fd),
        }
    }

    pub fn wait(&mut self, out: &mut Vec<Event>) -> io::Result<()> {
        let initial_len = out.len();
        while out.len() == initial_len {
            let now = Self::now_ms();
            // shortcut: linear timer scan, fine for < ~32 timers; switch to a heap if timer count grows
            let next_deadline = self.timers.iter().map(|t| t.expires_ms).min();
            let timeout = next_deadline.map(|deadline| {
                let ms = deadline.saturating_sub(now).min(i32::MAX as u64) as i64;
                rustix::time::Timespec {
                    tv_sec: ms / 1000,
                    tv_nsec: (ms % 1000) * 1_000_000,
                }
            });

            let mut buf = [std::mem::MaybeUninit::<epoll::Event>::uninit(); 64];
            let ready = match epoll::wait(&self.epoll, &mut buf, timeout.as_ref()) {
                Ok((ready, _)) => ready,
                Err(Errno::INTR) => {
                    self.process_timers(Self::now_ms(), out);
                    continue;
                }
                Err(e) => return Err(e.into()),
            };

            for ev in ready.iter() {
                let fd = ev.data.u64() as RawFd;
                let events = ev.flags.bits();

                if fd == self.event_fd.as_raw_fd() {
                    let mut val = [0u8; 8];
                    // Drain until EAGAIN; any other error just ends the drain like the C.
                    while let Ok(8) | Err(Errno::INTR) = rustix::io::read(&*self.event_fd, &mut val)
                    {
                    }
                    out.push(Event::Wakeup);
                } else if fd == self.signal_fd.as_raw_fd() {
                    loop {
                        let mut siginfo = std::mem::MaybeUninit::<libc::signalfd_siginfo>::uninit();
                        let size = std::mem::size_of::<libc::signalfd_siginfo>();
                        // SAFETY: reading at most size bytes into siginfo from our signalfd.
                        let res = unsafe {
                            libc::read(
                                self.signal_fd.as_raw_fd(),
                                siginfo.as_mut_ptr() as *mut libc::c_void,
                                size,
                            )
                        };
                        if res < 0 && io::Error::last_os_error().raw_os_error() == Some(libc::EINTR)
                        {
                            continue;
                        }
                        if res as usize != size {
                            break;
                        }
                        // SAFETY: the kernel filled the whole struct.
                        let siginfo = unsafe { siginfo.assume_init() };
                        self.stats.signals_received += 1;
                        out.push(Event::Signal(siginfo.ssi_signo as i32));
                    }
                } else if let Some(&token) = self.fd_tokens.get(&fd) {
                    self.stats.events_processed += 1;
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
        self.stats.timers_fired += due_indices.len() as u64;

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
        // SAFETY: restoring the signal mask saved in new(); the fds close via OwnedFd.
        unsafe {
            libc::sigprocmask(libc::SIG_SETMASK, &self.orig_sigmask, std::ptr::null_mut());
        }
    }
}
