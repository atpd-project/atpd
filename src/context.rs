//! ATPD Daemon lifecycle and VPN observation state context.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum VpnState {
    Idle = 0,
    Predicting = 1,
    Ready = 2,
    Teardown = 3,
}

impl VpnState {
    pub fn as_str(&self) -> &'static str {
        match self {
            VpnState::Idle => "IDLE",
            VpnState::Predicting => "PREDICTING",
            VpnState::Ready => "READY",
            VpnState::Teardown => "TEARDOWN",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum RuntimeState {
    Uninitialized = 0,
    Initializing = 1,
    Running = 2,
    Reloading = 3,
    Stopping = 4,
    Stopped = 5,
    Failed = 6,
}

impl RuntimeState {
    pub fn as_str(&self) -> &'static str {
        match self {
            RuntimeState::Uninitialized => "UNINITIALIZED",
            RuntimeState::Initializing => "INITIALIZING",
            RuntimeState::Running => "RUNNING",
            RuntimeState::Reloading => "RELOADING",
            RuntimeState::Stopping => "STOPPING",
            RuntimeState::Stopped => "STOPPED",
            RuntimeState::Failed => "FAILED",
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct VpnSnapshot {
    pub state: VpnState,
    pub if_id: u32,
    pub iface: [u8; 32],
    pub changed_at: libc::timespec,
    pub transitions: u64,
}

impl PartialEq for VpnSnapshot {
    fn eq(&self, other: &Self) -> bool {
        self.state == other.state
            && self.if_id == other.if_id
            && self.iface == other.iface
            && self.changed_at.tv_sec == other.changed_at.tv_sec
            && self.changed_at.tv_nsec == other.changed_at.tv_nsec
            && self.transitions == other.transitions
    }
}

impl Eq for VpnSnapshot {}

impl VpnSnapshot {
    pub fn iface_str(&self) -> &str {
        let len = self
            .iface
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(self.iface.len());
        std::str::from_utf8(&self.iface[..len]).unwrap_or("")
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VpnChange {
    pub state: VpnState,
    pub iface: Option<String>,
    pub teardown: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ContextError {
    AlreadyInitialized,
    InvalidTransition {
        from: RuntimeState,
        to: RuntimeState,
    },
    InvalidInterfaceName,
}

impl std::fmt::Display for ContextError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ContextError::AlreadyInitialized => {
                write!(f, "ATPD context initialization requested more than once")
            }
            ContextError::InvalidTransition { from, to } => {
                write!(
                    f,
                    "Rejected runtime transition {} -> {}",
                    from.as_str(),
                    to.as_str()
                )
            }
            ContextError::InvalidInterfaceName => write!(f, "Invalid VPN interface name"),
        }
    }
}

impl std::error::Error for ContextError {}

fn get_monotonic_timespec() -> libc::timespec {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: ts is a valid stack-allocated timespec struct.
    let ret = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    if ret != 0 {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    ts
}

/// Owned daemon context holding VPN and runtime state.
pub struct AtpdContext {
    initialized: bool,
    vpn: VpnSnapshot,
    runtime_state: RuntimeState,
}

impl AtpdContext {
    pub fn new() -> Self {
        Self {
            initialized: false,
            vpn: VpnSnapshot {
                state: VpnState::Idle,
                if_id: 0,
                iface: [0; 32],
                changed_at: libc::timespec {
                    tv_sec: 0,
                    tv_nsec: 0,
                },
                transitions: 0,
            },
            runtime_state: RuntimeState::Uninitialized,
        }
    }

    pub fn init(&mut self) -> Result<(), ContextError> {
        if self.initialized {
            return Err(ContextError::AlreadyInitialized);
        }
        let now = get_monotonic_timespec();
        self.initialized = true;
        self.vpn.state = VpnState::Idle;
        self.vpn.if_id = 0;
        self.vpn.iface = [0; 32];
        self.vpn.changed_at = now;
        self.vpn.transitions = 0;
        self.runtime_state = RuntimeState::Uninitialized;
        Ok(())
    }

    pub fn runtime_transition_allowed(old_state: RuntimeState, new_state: RuntimeState) -> bool {
        match old_state {
            RuntimeState::Uninitialized => new_state == RuntimeState::Initializing,
            RuntimeState::Initializing => {
                new_state == RuntimeState::Running
                    || new_state == RuntimeState::Failed
                    || new_state == RuntimeState::Stopping
            }
            RuntimeState::Running => {
                new_state == RuntimeState::Reloading
                    || new_state == RuntimeState::Stopping
                    || new_state == RuntimeState::Failed
            }
            RuntimeState::Reloading => {
                new_state == RuntimeState::Running || new_state == RuntimeState::Stopping
            }
            RuntimeState::Stopping => new_state == RuntimeState::Stopped,
            RuntimeState::Failed => {
                new_state == RuntimeState::Stopping || new_state == RuntimeState::Stopped
            }
            RuntimeState::Stopped => false,
        }
    }

    pub fn runtime_state_transition(
        &mut self,
        new_state: RuntimeState,
    ) -> Result<(), ContextError> {
        let old_state = self.runtime_state;
        if old_state == new_state {
            return Ok(());
        }
        if !Self::runtime_transition_allowed(old_state, new_state) {
            return Err(ContextError::InvalidTransition {
                from: old_state,
                to: new_state,
            });
        }
        self.runtime_state = new_state;
        crate::log_info!(
            "RUNTIME_STATE: {} -> {}",
            old_state.as_str(),
            new_state.as_str()
        );
        Ok(())
    }

    pub fn can_reload(&self) -> bool {
        self.runtime_state == RuntimeState::Running
    }

    pub fn vpn_snapshot(&self) -> VpnSnapshot {
        self.vpn
    }

    pub fn vpn_transition(
        &mut self,
        new_state: VpnState,
        if_id: u32,
        iface: Option<&str>,
    ) -> Result<Option<VpnChange>, ContextError> {
        let iface_clean = iface.unwrap_or("").trim();
        if iface_clean.len() >= 32 {
            return Err(ContextError::InvalidInterfaceName);
        }

        let old_state = self.vpn.state;
        let mut new_iface_buf = [0u8; 32];
        new_iface_buf[..iface_clean.len()].copy_from_slice(iface_clean.as_bytes());

        let identity_changed = self.vpn.if_id != if_id || self.vpn.iface != new_iface_buf;

        if old_state == new_state && !identity_changed {
            return Ok(None);
        }

        let now = get_monotonic_timespec();
        self.vpn.state = new_state;
        self.vpn.if_id = if_id;
        self.vpn.iface = new_iface_buf;
        self.vpn.changed_at = now;
        self.vpn.transitions = self.vpn.transitions.saturating_add(1);

        let teardown_edge = old_state != VpnState::Teardown && new_state == VpnState::Teardown;
        crate::log_info!(
            "VPN_STATE: {} -> {} (IF_ID={})",
            old_state.as_str(),
            new_state.as_str(),
            if_id
        );
        if teardown_edge {
            crate::log_warn!("VPN_STATE: teardown observer requested session cleanup");
        }

        let iface_opt = if iface_clean.is_empty() {
            None
        } else {
            Some(iface_clean.to_string())
        };

        Ok(Some(VpnChange {
            state: new_state,
            iface: iface_opt,
            teardown: teardown_edge,
        }))
    }
}

impl Default for AtpdContext {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_atpd_context_ported() {
        let mut ctx = AtpdContext::new();

        assert_eq!(ctx.init(), Ok(()));
        assert_eq!(ctx.init(), Err(ContextError::AlreadyInitialized));

        assert_eq!(
            ctx.runtime_state_transition(RuntimeState::Initializing),
            Ok(())
        );
        assert!(!ctx.can_reload());
        assert_eq!(ctx.runtime_state_transition(RuntimeState::Running), Ok(()));

        assert_eq!(
            ctx.runtime_state_transition(RuntimeState::Reloading),
            Ok(())
        );
        assert!(!ctx.can_reload());
        assert_eq!(ctx.runtime_state_transition(RuntimeState::Running), Ok(()));

        assert_eq!(ctx.runtime_state_transition(RuntimeState::Stopping), Ok(()));
        assert_eq!(ctx.runtime_state_transition(RuntimeState::Stopped), Ok(()));
        assert_eq!(
            ctx.runtime_state_transition(RuntimeState::Running),
            Err(ContextError::InvalidTransition {
                from: RuntimeState::Stopped,
                to: RuntimeState::Running,
            })
        );

        let res = ctx.vpn_transition(VpnState::Ready, 1, Some("ipsec0"));
        assert!(res.is_ok());
        let change = res.unwrap();
        assert_eq!(
            change,
            Some(VpnChange {
                state: VpnState::Ready,
                iface: Some("ipsec0".to_string()),
                teardown: false,
            })
        );
        let snapshot = ctx.vpn_snapshot();
        assert_eq!(snapshot.state, VpnState::Ready);
        assert_eq!(snapshot.if_id, 1);
        assert_eq!(snapshot.transitions, 1);
        assert_eq!(snapshot.iface_str(), "ipsec0");

        let res2 = ctx.vpn_transition(VpnState::Ready, 2, Some("ipsec0"));
        assert!(res2.is_ok());
        let change2 = res2.unwrap();
        assert_eq!(
            change2,
            Some(VpnChange {
                state: VpnState::Ready,
                iface: Some("ipsec0".to_string()),
                teardown: false,
            })
        );
        let snapshot2 = ctx.vpn_snapshot();
        assert_eq!(snapshot2.if_id, 2);
        assert_eq!(snapshot2.transitions, 2);

        // Duplicate transition returns Ok(None)
        let dup = ctx.vpn_transition(VpnState::Ready, 2, Some("ipsec0"));
        assert_eq!(dup, Ok(None));
        assert_eq!(ctx.vpn_snapshot().transitions, 2);

        // Teardown transition triggers teardown: true
        let teardown = ctx.vpn_transition(VpnState::Teardown, 0, None);
        assert_eq!(
            teardown,
            Ok(Some(VpnChange {
                state: VpnState::Teardown,
                iface: None,
                teardown: true,
            }))
        );
        assert_eq!(ctx.vpn_snapshot().transitions, 3);

        // Second teardown is duplicate or non-edge teardown
        let dup_teardown = ctx.vpn_transition(VpnState::Teardown, 0, None);
        assert_eq!(dup_teardown, Ok(None));
    }
}
