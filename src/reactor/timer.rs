#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TimerId(pub u64);

// shortcut: linear timer scan, fine for < ~32 timers; switch to a heap if timer count grows
#[derive(Debug, Clone)]
pub struct TimerEntry {
    pub id: TimerId,
    pub expires_ms: u64,
    pub interval_ms: u64,
}
