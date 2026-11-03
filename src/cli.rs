//! CLI argument parsing and help/version display.
//!
//! Mirrors `src/cli.c` and `include/cli.h`.

use std::fmt;

pub const EXIT_FAILURE: i32 = 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Command {
    None,
    Start,
    Stop,
    Restart,
    Status,
    Reload,
    Check,
    Version,
    Help,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RunMode {
    Default,
    Foreground,
    Daemon,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verbosity {
    Default,
    Verbose,
    Quiet,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CliOptions {
    pub command: Command,
    pub config_file: String,
    pub pid_file: String,
    pub run_mode: RunMode,
    pub verbosity: Verbosity,
    pub no_color: bool,
}

impl Default for CliOptions {
    fn default() -> Self {
        Self {
            command: Command::None,
            config_file: String::new(),
            pid_file: String::new(),
            run_mode: RunMode::Default,
            verbosity: Verbosity::Default,
            no_color: false,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CliError {
    pub message: String,
    pub exit_code: i32,
}

impl fmt::Display for CliError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for CliError {}

impl CliError {
    pub fn new(msg: impl Into<String>) -> Self {
        Self {
            message: msg.into(),
            exit_code: EXIT_FAILURE,
        }
    }
}

fn command_from_string(name: &str) -> Option<Command> {
    match name {
        "start" => Some(Command::Start),
        "stop" => Some(Command::Stop),
        "restart" => Some(Command::Restart),
        "status" => Some(Command::Status),
        "reload" => Some(Command::Reload),
        "check" => Some(Command::Check),
        "version" => Some(Command::Version),
        "help" => Some(Command::Help),
        _ => None,
    }
}

const OPTION_CONFIG: u32 = 1 << 0;
const OPTION_PID: u32 = 1 << 1;
const OPTION_FOREGROUND: u32 = 1 << 2;
const OPTION_DAEMON: u32 = 1 << 3;
const OPTION_VERBOSE: u32 = 1 << 4;
const OPTION_QUIET: u32 = 1 << 5;
const OPTION_NO_COLOR: u32 = 1 << 6;
const OPTION_HELP: u32 = 1 << 7;
const OPTION_VERSION: u32 = 1 << 8;

const PATH_MAX: usize = 4096;

pub fn parse_arguments(args: &[String]) -> Result<CliOptions, CliError> {
    let tokens =
        if !args.is_empty() && !args[0].starts_with('-') && command_from_string(&args[0]).is_none()
        {
            &args[1..]
        } else {
            args
        };

    let mut opts = CliOptions::default();
    let mut seen = 0u32;
    let mut explicit_command: Option<Command> = None;
    let mut positional_command: Option<Command> = None;

    let mut idx = 0;
    while idx < tokens.len() {
        let arg = &tokens[idx];

        if arg == "--" {
            idx += 1;
            while idx < tokens.len() {
                if positional_command.is_some() {
                    return Err(CliError::new(format!(
                        "unknown or trailing command argument: {}",
                        tokens[idx]
                    )));
                }
                match command_from_string(&tokens[idx]) {
                    Some(cmd) => positional_command = Some(cmd),
                    None => {
                        return Err(CliError::new(format!(
                            "unknown or trailing command argument: {}",
                            tokens[idx]
                        )))
                    }
                }
                idx += 1;
            }
            break;
        }

        if arg.starts_with("--") {
            let (key, value) = match arg.split_once('=') {
                Some((k, v)) => (k, Some(v.to_string())),
                None => (arg.as_str(), None),
            };

            match key {
                "--config" => {
                    let val = match value {
                        Some(v) => v,
                        None => {
                            idx += 1;
                            if idx >= tokens.len() {
                                return Err(CliError::new(
                                    "option '--config' requires an argument",
                                ));
                            }
                            tokens[idx].clone()
                        }
                    };
                    if val.len() >= PATH_MAX {
                        return Err(CliError::new("--config: path too long"));
                    }
                    opts.config_file = val;
                    seen |= OPTION_CONFIG;
                }
                "--pid" => {
                    let val = match value {
                        Some(v) => v,
                        None => {
                            idx += 1;
                            if idx >= tokens.len() {
                                return Err(CliError::new("option '--pid' requires an argument"));
                            }
                            tokens[idx].clone()
                        }
                    };
                    if val.len() >= PATH_MAX {
                        return Err(CliError::new("--pid: path too long"));
                    }
                    opts.pid_file = val;
                    seen |= OPTION_PID;
                }
                "--foreground" => {
                    opts.run_mode = RunMode::Foreground;
                    seen |= OPTION_FOREGROUND;
                }
                "--daemon" => {
                    opts.run_mode = RunMode::Daemon;
                    seen |= OPTION_DAEMON;
                }
                "--verbose" => {
                    opts.verbosity = Verbosity::Verbose;
                    seen |= OPTION_VERBOSE;
                }
                "--quiet" => {
                    opts.verbosity = Verbosity::Quiet;
                    seen |= OPTION_QUIET;
                }
                "--no-color" => {
                    opts.no_color = true;
                    seen |= OPTION_NO_COLOR;
                }
                "--help" => {
                    explicit_command = Some(Command::Help);
                    seen |= OPTION_HELP;
                }
                "--version" => {
                    explicit_command = Some(Command::Version);
                    seen |= OPTION_VERSION;
                }
                _ => return Err(CliError::new(format!("unknown option: {}", key))),
            }
        } else if arg.starts_with('-') && arg.len() > 1 {
            for (c_idx, c) in arg.char_indices().skip(1) {
                match c {
                    'c' => {
                        let remaining = &arg[c_idx + c.len_utf8()..];
                        let val = if !remaining.is_empty() {
                            remaining.to_string()
                        } else {
                            idx += 1;
                            if idx >= tokens.len() {
                                return Err(CliError::new("option '-c' requires an argument"));
                            }
                            tokens[idx].clone()
                        };
                        if val.len() >= PATH_MAX {
                            return Err(CliError::new("--config: path too long"));
                        }
                        opts.config_file = val;
                        seen |= OPTION_CONFIG;
                        break;
                    }
                    'p' => {
                        let remaining = &arg[c_idx + c.len_utf8()..];
                        let val = if !remaining.is_empty() {
                            remaining.to_string()
                        } else {
                            idx += 1;
                            if idx >= tokens.len() {
                                return Err(CliError::new("option '-p' requires an argument"));
                            }
                            tokens[idx].clone()
                        };
                        if val.len() >= PATH_MAX {
                            return Err(CliError::new("--pid: path too long"));
                        }
                        opts.pid_file = val;
                        seen |= OPTION_PID;
                        break;
                    }
                    'f' => {
                        opts.run_mode = RunMode::Foreground;
                        seen |= OPTION_FOREGROUND;
                    }
                    'd' => {
                        opts.run_mode = RunMode::Daemon;
                        seen |= OPTION_DAEMON;
                    }
                    'V' => {
                        opts.verbosity = Verbosity::Verbose;
                        seen |= OPTION_VERBOSE;
                    }
                    'q' => {
                        opts.verbosity = Verbosity::Quiet;
                        seen |= OPTION_QUIET;
                    }
                    'n' => {
                        opts.no_color = true;
                        seen |= OPTION_NO_COLOR;
                    }
                    'h' => {
                        explicit_command = Some(Command::Help);
                        seen |= OPTION_HELP;
                    }
                    'v' => {
                        explicit_command = Some(Command::Version);
                        seen |= OPTION_VERSION;
                    }
                    _ => return Err(CliError::new(format!("unknown option: -{}", c))),
                }
            }
        } else {
            if positional_command.is_some() {
                return Err(CliError::new(format!(
                    "unknown or trailing command argument: {}",
                    arg
                )));
            }
            match command_from_string(arg) {
                Some(cmd) => positional_command = Some(cmd),
                None => {
                    return Err(CliError::new(format!(
                        "unknown or trailing command argument: {}",
                        arg
                    )))
                }
            }
        }

        idx += 1;
    }

    if let Some(cmd) = positional_command {
        opts.command = cmd;
    } else if let Some(cmd) = explicit_command {
        opts.command = cmd;
    } else {
        opts.command = Command::Help;
    }

    if let Some(cmd) = explicit_command {
        if opts.command != cmd {
            return Err(CliError::new(
                "help/version cannot be combined with a command",
            ));
        }
    }

    // Validation matching C validate_options
    let mode = seen & (OPTION_FOREGROUND | OPTION_DAEMON);
    let verbosity = seen & (OPTION_VERBOSE | OPTION_QUIET);

    if mode == (OPTION_FOREGROUND | OPTION_DAEMON) {
        return Err(CliError::new(
            "--foreground and --daemon are mutually exclusive",
        ));
    }
    if verbosity == (OPTION_VERBOSE | OPTION_QUIET) {
        return Err(CliError::new(
            "--verbose and --quiet are mutually exclusive",
        ));
    }
    if opts.command != Command::Start && opts.command != Command::Restart && mode != 0 {
        return Err(CliError::new("run mode is only valid with start/restart"));
    }
    if (opts.command == Command::Version || opts.command == Command::Help)
        && (seen & (OPTION_CONFIG | OPTION_PID | OPTION_VERBOSE | OPTION_QUIET | OPTION_NO_COLOR))
            != 0
    {
        return Err(CliError::new("options are not valid with version/help"));
    }
    if opts.command == Command::Check && (seen & OPTION_PID) != 0 {
        return Err(CliError::new("--pid is not valid with check"));
    }
    if (seen & (OPTION_HELP | OPTION_VERSION)) != 0 {
        let expected = if (seen & OPTION_HELP) != 0 {
            Command::Help
        } else {
            Command::Version
        };
        if opts.command != expected {
            return Err(CliError::new(
                "help/version cannot be combined with a command",
            ));
        }
    }

    Ok(opts)
}

pub fn print_usage(progname: &str) -> String {
    let base = progname
        .rsplit('/')
        .next()
        .filter(|s| !s.is_empty())
        .unwrap_or(if progname.is_empty() {
            "atpd"
        } else {
            progname
        });

    format!(
        "atpd {}\n\nUsage: {} [options] command\n\nOptions:\n  -c, --config FILE     Specify configuration file\n  -p, --pid FILE        Specify PID file path\n  -d, --daemon          Run as daemon (default for start)\n  -f, --foreground      Run in foreground (do not daemonize)\n  -V, --verbose         Verbose output (debug level)\n  -q, --quiet           Quiet output (errors only)\n  -n, --no-color        Disable colored output\n  -h, --help            Show this help\n  -v, --version         Print version and exit\n\nCommands:\n  start                 Start daemon\n  stop                  Stop daemon\n  restart               Restart daemon\n  status                Show daemon, VPN tunnel, and traffic status\n  reload                Reload configuration without restart\n  check                 Validate configuration and exit\n  help                  Show this help message\n\nExamples:\n  {} status\n",
        crate::version::get_full_version(),
        base,
        base
    )
}

pub fn print_version() -> String {
    format!("atpd {}\n", crate::version::get_full_version())
}

#[cfg(test)]
mod tests {
    use super::*;

    // Ported from tests/test_cli.c: expect_custom_config_command
    fn expect_custom_config_command(command: &str, expected: Command) {
        let argv = vec![
            "atpd".to_string(),
            "-c".to_string(),
            "/tmp/custom.conf".to_string(),
            command.to_string(),
        ];
        let opts = parse_arguments(&argv).expect("parse_arguments should succeed");
        assert_eq!(opts.command, expected);
        assert_eq!(opts.config_file, "/tmp/custom.conf");
    }

    #[test]
    fn test_cli_ported_from_test_cli_c() {
        expect_custom_config_command("stop", Command::Stop);
        expect_custom_config_command("status", Command::Status);
        expect_custom_config_command("reload", Command::Reload);

        let trailing = vec![
            "atpd".to_string(),
            "status".to_string(),
            "extra".to_string(),
        ];
        assert!(parse_arguments(&trailing).is_err());

        let invalid_mode = vec![
            "atpd".to_string(),
            "--foreground".to_string(),
            "status".to_string(),
        ];
        assert!(parse_arguments(&invalid_mode).is_err());
    }

    #[test]
    fn test_cli_flags_and_mutual_exclusion() {
        // Foreground and daemon mutually exclusive
        let mutual_exclusive = vec![
            "atpd".to_string(),
            "--foreground".to_string(),
            "--daemon".to_string(),
            "start".to_string(),
        ];
        assert!(parse_arguments(&mutual_exclusive).is_err());

        // Verbose and quiet mutually exclusive
        let vq_exclusive = vec![
            "atpd".to_string(),
            "-V".to_string(),
            "-q".to_string(),
            "start".to_string(),
        ];
        assert!(parse_arguments(&vq_exclusive).is_err());

        // Pid not valid with check
        let check_pid = vec![
            "atpd".to_string(),
            "-p".to_string(),
            "/run/atpd.pid".to_string(),
            "check".to_string(),
        ];
        assert!(parse_arguments(&check_pid).is_err());

        // Options not valid with help/version
        let version_with_config = vec![
            "atpd".to_string(),
            "-c".to_string(),
            "/tmp/c.conf".to_string(),
            "-v".to_string(),
        ];
        assert!(parse_arguments(&version_with_config).is_err());

        // Help default when no arguments
        let empty: Vec<String> = vec![];
        let opts = parse_arguments(&empty).unwrap();
        assert_eq!(opts.command, Command::Help);

        // Version flag
        let ver = vec!["-v".to_string()];
        let opts = parse_arguments(&ver).unwrap();
        assert_eq!(opts.command, Command::Version);
    }
}
