// SPDX-License-Identifier: Apache-2.0
//! Bundled extension for the `terminal-extensions:statusline` world: the system's load, refreshed every
//! few seconds.
//!
//! A module is the same on every operating system, so its manifest declares one command per kind of
//! system, and the extension probes them in the order of [`PROBES`] until one answers with output it can
//! read: `uptime` on the Unix systems (`load 0.52 0.58 0.59`, the 1, 5 and 15 minute load averages), and
//! the processor load PowerShell reports on Windows (`cpu 17%`). The probe that answered is used from
//! then on, until it stops answering. While the commands are not allowed, it shows nothing.
//!
//! The load is read at load, whether the pane is focused or not, so it shows at once; the periodic
//! refresh runs only while the pane is focused, and gaining the focus reads it again.
//!
//! Settings:
//! - `refresh-ms`: how often the load is read while the pane is focused, in milliseconds (default 5000).
//!
//! Built for `wasm32-wasip1` as a plain core module, never componentized.

use std::cell::{Cell, RefCell};

use crate::terminal_extensions::statusline::host;

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

/// The key of the setting that holds the refresh interval.
const REFRESH_KEY: &str = "refresh-ms";
const DEFAULT_REFRESH_MS: u32 = 5000;

/// One way of reading the load: a command the manifest declares, and what reads its output.
struct Probe {
    command: &'static str,
    describe: fn(&str) -> Option<String>,
}

/// Every way of reading the load, in the order they are tried.
const PROBES: [Probe; 2] = [
    Probe {
        command: "uptime",
        describe: describe_uptime,
    },
    Probe {
        command: "cpu-windows",
        describe: describe_percentage,
    },
];

/// Whether the pane is focused: the periodic refresh runs only while it is.
#[derive(Clone, Copy, PartialEq)]
enum Focus {
    Unfocused,
    Focused,
}

thread_local! {
    /// Whether the pane is focused. A guest instance is single-threaded.
    static FOCUS: Cell<Focus> = const { Cell::new(Focus::Unfocused) };

    /// The probe that answered last; the first to try.
    static PROBE: Cell<usize> = const { Cell::new(0) };

    /// The refresh interval in milliseconds.
    static REFRESH_MS: Cell<u32> = const { Cell::new(DEFAULT_REFRESH_MS) };

    /// What `render` writes.
    static SHOWN: RefCell<String> = const { RefCell::new(String::new()) };
}

/// Reads the load averages off what `uptime` prints, whatever its platform's wording: the three values
/// after the last colon, separated by blanks and commas (`load average: 0.52, 0.58, 0.59` on Linux,
/// `load averages: 1.23 1.45 1.67` on macOS). A comma followed by a digit is a decimal comma.
/// @return `load <1> <5> <15>`, or nothing if @p output is not that.
fn describe_uptime(output: &str) -> Option<String> {
    let (_, averages) = output.trim().rsplit_once(':')?;
    let values: Vec<&str> = averages
        .split(|c: char| c.is_whitespace())
        .map(|value| value.trim_end_matches(','))
        .filter(|value| !value.is_empty())
        .collect();
    let numeric = |value: &&str| {
        value
            .chars()
            .all(|c| c.is_ascii_digit() || c == '.' || c == ',')
    };
    (values.len() == 3 && values.iter().all(numeric)).then(|| format!("load {}", values.join(" ")))
}

/// Reads a whole percentage, the first line of @p output (one per processor socket).
/// @return `cpu <percent>%`, or nothing if @p output is not that.
fn describe_percentage(output: &str) -> Option<String> {
    let first = output
        .lines()
        .map(str::trim)
        .find(|line| !line.is_empty())?;
    let percent: u32 = first.parse().ok()?;
    (percent <= 100).then(|| format!("cpu {percent}%"))
}

/// What one probe found.
enum Outcome {
    /// The text to show.
    Shown(String),
    /// Nothing this probe can read: the next one may do better.
    TryNext,
    /// The commands are not allowed (yet), so no other probe would be either.
    Refused,
}

/// Runs @p probe.
fn run(probe: &Probe) -> Outcome {
    match host::run_command(probe.command, &[]) {
        Ok(output) => match (probe.describe)(&String::from_utf8_lossy(&output.stdout)) {
            Some(text) => Outcome::Shown(text),
            None => Outcome::TryNext,
        },
        Err(
            host::CommandError::Failed
            | host::CommandError::TimedOut
            | host::CommandError::NotDeclared,
        ) => Outcome::TryNext,
        Err(host::CommandError::Pending | host::CommandError::Denied) => Outcome::Refused,
    }
}

/// Reads the load, starting with the probe that answered last, and remembers it for `render`.
/// @return Whether what is shown changed.
fn refresh() -> bool {
    let first = PROBE.get();
    let mut now = String::new();
    for index in (0..PROBES.len()).map(|offset| (first + offset) % PROBES.len()) {
        match run(&PROBES[index]) {
            Outcome::Shown(text) => {
                PROBE.set(index);
                now = text;
                break;
            }
            Outcome::TryNext => continue,
            Outcome::Refused => break,
        }
    }
    SHOWN.with(|shown| {
        let changed = *shown.borrow() != now;
        *shown.borrow_mut() = now;
        changed
    })
}

/// Asks for the next refresh, while the pane is focused.
fn schedule() {
    if FOCUS.get() == Focus::Focused {
        host::request_frame(REFRESH_MS.get());
    }
}

/// @return `render` if @p changed, else `none`.
fn render_if(changed: bool) -> RenderRequest {
    if changed {
        RenderRequest::Render
    } else {
        RenderRequest::None
    }
}

struct Sysload;

impl Guest for Sysload {
    fn load(config: Vec<(String, String)>) -> EventKinds {
        let interval = config
            .iter()
            .find(|(key, _)| key == REFRESH_KEY)
            .and_then(|(_, value)| value.trim().parse().ok());
        if let Some(interval) = interval {
            REFRESH_MS.set(interval);
        }
        FOCUS.set(if host::state().focused {
            Focus::Focused
        } else {
            Focus::Unfocused
        });
        refresh();
        schedule();
        EventKinds::FOCUS
    }

    fn update(ev: Event) -> RenderRequest {
        match ev {
            Event::Timer if FOCUS.get() == Focus::Focused => {
                schedule();
                render_if(refresh())
            }
            Event::Focus(true) => {
                let was = FOCUS.replace(Focus::Focused);
                if was == Focus::Focused {
                    return RenderRequest::None;
                }
                schedule();
                render_if(refresh())
            }
            Event::Focus(false) => {
                FOCUS.set(Focus::Unfocused);
                RenderRequest::None
            }
            _ => RenderRequest::None,
        }
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        SHOWN.with(|shown| shown.borrow().clone().into_bytes())
    }
}

export!(Sysload);

// The commands the host may run on this extension's behalf, one per kind of system. The host runs a command
// through the platform's shell, and refuses an argument that shell would interpret rather than quote it --
// cmd.exe's parentheses among them. So PowerShell is handed its script encoded (-EncodedCommand: base64 of
// the script's UTF-16LE), which only base64 characters spell:
// `(Get-CimInstance Win32_Processor).LoadPercentage`.
manifest::declare_manifest!(
    "name = sysload\nversion = 1\n\
     command uptime = uptime\n\
     command cpu-windows = powershell -NoProfile -NonInteractive -EncodedCommand \
     KABHAGUAdAAtAEMAaQBtAEkAbgBzAHQAYQBuAGMAZQAgAFcAaQBuADMAMgBfAFAAcgBvAGMAZQBzAHMAbwByACkALgBMAG8AYQBkAFAAZQByAGMAZQBuAHQAYQBnAGUA\n"
);
