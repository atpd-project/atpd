use super::*;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

// Mirrors tests/test_reactor_timers.c: run_one_shot
#[test]
fn test_reactor_one_shot() {
    let mut reactor = Reactor::new().expect("create reactor");
    let mut events = Vec::new();
    let timer_id = reactor.add_timer(10, 0);

    reactor.wait(&mut events).expect("wait for event");
    assert_eq!(events.len(), 1);
    assert_eq!(events[0], Event::Timer(timer_id));

    // A subsequent wait with a short timeout should not re-fire the one-shot timer
    events.clear();
    let next_id = reactor.add_timer(20, 0);
    reactor.wait(&mut events).expect("wait for next event");
    assert_eq!(events.len(), 1);
    assert_eq!(events[0], Event::Timer(next_id));
}

// Mirrors tests/test_reactor_timers.c: run_repeated
#[test]
fn test_reactor_repeated() {
    let mut reactor = Reactor::new().expect("create reactor");
    let mut events = Vec::new();
    let timer_id = reactor.add_timer(10, 10);

    let mut fired_count = 0;
    while fired_count < 3 {
        events.clear();
        reactor.wait(&mut events).expect("wait for event");
        for ev in &events {
            if *ev == Event::Timer(timer_id) {
                fired_count += 1;
            }
        }
    }
    assert_eq!(fired_count, 3);
}

// Mirrors tests/test_reactor_timers.c: run_cancelled
#[test]
fn test_reactor_cancelled() {
    let mut reactor = Reactor::new().expect("create reactor");
    let mut events = Vec::new();
    let timer1 = reactor.add_timer(10, 0);
    assert!(reactor.cancel_timer(timer1));
    // Cancelling a second time should return false
    assert!(!reactor.cancel_timer(timer1));

    let timer2 = reactor.add_timer(30, 0);
    reactor.wait(&mut events).expect("wait for event");
    assert_eq!(events.len(), 1);
    assert_eq!(events[0], Event::Timer(timer2));
}

// Covers timer ordering across deadlines
#[test]
fn test_reactor_timer_ordering() {
    let mut reactor = Reactor::new().expect("create reactor");
    let mut events = Vec::new();

    // Add timers in out-of-order deadlines: 40ms, 10ms, 25ms
    let t3 = reactor.add_timer(40, 0);
    let t1 = reactor.add_timer(10, 0);
    let t2 = reactor.add_timer(25, 0);

    let mut fired = Vec::new();
    while fired.len() < 3 {
        events.clear();
        reactor.wait(&mut events).expect("wait for event");
        for ev in &events {
            if let Event::Timer(id) = *ev {
                fired.push(id);
            }
        }
    }

    assert_eq!(fired, vec![t1, t2, t3]);
}

// Covers waker wakes a blocked wait from another thread
#[test]
fn test_reactor_waker_wakes_blocked_wait() {
    let mut reactor = Reactor::new().expect("create reactor");
    let waker = reactor.waker();
    let woken = Arc::new(AtomicBool::new(false));
    let woken_clone = woken.clone();

    let handle = thread::spawn(move || {
        thread::sleep(Duration::from_millis(30));
        woken_clone.store(true, Ordering::SeqCst);
        waker.wake().expect("waker wake");
    });

    let mut events = Vec::new();
    reactor.wait(&mut events).expect("wait unblocked by waker");
    assert!(woken.load(Ordering::SeqCst));
    assert!(events.contains(&Event::Wakeup));

    handle.join().expect("join waker thread");
}

// Regression: a Waker outliving its Reactor must not write into a reused fd number
#[test]
fn test_waker_outliving_reactor_does_not_hit_reused_fd() {
    let reactor = Reactor::new().expect("create reactor");
    let waker = reactor.waker();
    drop(reactor);

    let mut fds = [0; 2];
    // SAFETY: fds is a valid 2-element array for pipe2.
    assert_eq!(
        unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_NONBLOCK) },
        0
    );
    waker.wake().expect("wake after reactor drop");

    let mut buf = [0u8; 8];
    // SAFETY: reading into a valid local buffer from our own pipe.
    let n = unsafe { libc::read(fds[0], buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
    // SAFETY: closing the pipe fds created above.
    unsafe {
        libc::close(fds[0]);
        libc::close(fds[1]);
    }
    assert_eq!(n, -1, "wake() wrote into an unrelated pipe");
}

// Covers watched signal delivery as Event::Signal
#[test]
fn test_reactor_watched_signal() {
    let mut reactor = Reactor::new().expect("create reactor");
    reactor.watch_signal(libc::SIGUSR1).expect("watch SIGUSR1");

    // SAFETY: raising SIGUSR1 to the calling thread for signalfd reception.
    let ret = unsafe { libc::raise(libc::SIGUSR1) };
    assert_eq!(ret, 0);

    let mut events = Vec::new();
    reactor.wait(&mut events).expect("wait for signal");
    assert!(events.contains(&Event::Signal(libc::SIGUSR1)));
}

// Covers pipe fd readable event delivery with its token
#[test]
fn test_reactor_fd_readable_pipe() {
    let mut reactor = Reactor::new().expect("create reactor");
    let mut fds = [0; 2];
    // SAFETY: creating pipe for I/O event testing.
    let ret = unsafe { libc::pipe(fds.as_mut_ptr()) };
    assert_eq!(ret, 0);
    let (read_fd, write_fd) = (fds[0], fds[1]);

    const TOKEN: u64 = 0xbeef;
    reactor
        .add_fd(read_fd, READ, TOKEN)
        .expect("add read fd to epoll");

    // Write a byte into the pipe
    let byte = b"x";
    let written =
        // SAFETY: writing one byte to write_fd.
        unsafe { libc::write(write_fd, byte.as_ptr() as *const libc::c_void, byte.len()) };
    assert_eq!(written, 1);

    let mut events = Vec::new();
    reactor.wait(&mut events).expect("wait for pipe read event");

    let io_event = events
        .iter()
        .find(|ev| matches!(ev, Event::Io { token, .. } if *token == TOKEN));
    assert!(io_event.is_some());
    if let Some(Event::Io { token, events: ev }) = io_event {
        assert_eq!(*token, TOKEN);
        assert_ne!(ev & READ, 0);
    }

    // Modify fd to WRITE and verify
    reactor.modify_fd(read_fd, WRITE).expect("modify fd events");

    // Remove fd
    reactor.remove_fd(read_fd).expect("remove read fd");

    // SAFETY: closing test pipe file descriptors.
    unsafe {
        libc::close(read_fd);
        libc::close(write_fd);
    }
}
