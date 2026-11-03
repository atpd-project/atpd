use std::time::SystemTime;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Backoff {
    pub base_delay_ms: u64,
    pub max_delay_ms: u64,
    pub current_delay_ms: u64,
    pub multiplier: u64,
}

impl Default for Backoff {
    fn default() -> Self {
        Self::new()
    }
}

impl Backoff {
    pub fn new() -> Self {
        Self {
            base_delay_ms: 1000,
            max_delay_ms: 60000,
            current_delay_ms: 1000,
            multiplier: 2,
        }
    }

    pub fn reset(&mut self) {
        self.current_delay_ms = self.base_delay_ms;
    }

    pub fn next_delay(&mut self) -> u64 {
        let delay = self.current_delay_ms;
        self.current_delay_ms = self.current_delay_ms.saturating_mul(self.multiplier);
        if self.current_delay_ms > self.max_delay_ms {
            self.current_delay_ms = self.max_delay_ms;
        }
        delay
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CircuitBreaker {
    pub consecutive_failures: u32,
    pub threshold: u32,
    pub cooldown_seconds: u64,
    pub last_failure_time: u64,
    pub circuit_open: bool,
}

impl CircuitBreaker {
    pub fn new(threshold: u32, cooldown_seconds: u64) -> Self {
        Self {
            consecutive_failures: 0,
            threshold: if threshold > 0 { threshold } else { 5 },
            cooldown_seconds: if cooldown_seconds > 0 {
                cooldown_seconds
            } else {
                60
            },
            last_failure_time: 0,
            circuit_open: false,
        }
    }

    pub fn should_allow_at(&mut self, now_sec: u64) -> bool {
        if !self.circuit_open {
            return true;
        }
        if now_sec.saturating_sub(self.last_failure_time) > self.cooldown_seconds {
            crate::log_info!("Circuit breaker: closed (cooldown expired)");
            self.circuit_open = false;
            self.consecutive_failures = 0;
            return true;
        }
        false
    }

    pub fn should_allow(&mut self) -> bool {
        self.should_allow_at(system_now_sec())
    }

    pub fn record_failure_at(&mut self, now_sec: u64) {
        self.consecutive_failures = self.consecutive_failures.saturating_add(1);
        self.last_failure_time = now_sec;
        if self.consecutive_failures >= self.threshold && !self.circuit_open {
            crate::log_warn!(
                "Circuit breaker: opened after {} failures",
                self.consecutive_failures
            );
            self.circuit_open = true;
        }
    }

    pub fn record_failure(&mut self) {
        self.record_failure_at(system_now_sec());
    }

    pub fn record_success(&mut self) {
        if self.consecutive_failures > 0 {
            self.consecutive_failures = 0;
        }
        if self.circuit_open {
            self.circuit_open = false;
        }
    }

    pub fn is_open(&self) -> bool {
        self.circuit_open
    }
}

pub fn system_now_sec() -> u64 {
    match SystemTime::now().duration_since(SystemTime::UNIX_EPOCH) {
        Ok(dur) => dur.as_secs(),
        Err(_) => 0,
    }
}
