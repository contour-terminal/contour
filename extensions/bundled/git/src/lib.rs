// SPDX-License-Identifier: Apache-2.0
//! Bundled extension for the `terminal-extensions:statusline` world: the branch of the git repository the
//! pane's working directory is in, followed by `*` when the working tree has changes.
//!
//! It asks git through the one command its manifest declares,
//! `git --no-optional-locks -C {cwd} status --porcelain=v2 --branch`. `--no-optional-locks` keeps a status
//! taken in the background from writing the index back, so it never holds the lock a git command the
//! user runs meanwhile needs.
//!
//! The status is taken when something can have changed it, not on a clock: at load, when the working
//! directory changes, when the foreground process changes (a command finished, or started), and when the
//! pane gains the focus (for what changed meanwhile, here or elsewhere). These run whether the pane is
//! focused or not, so an unfocused split shows its branch too; only the opt-in periodic refresh waits for
//! the focus. Outside a repository, and while the command is not allowed, it shows nothing; a command that
//! fails is logged, once per kind of failure.
//!
//! Settings:
//! - `refresh-ms`: also take the status every so many milliseconds, while the pane is focused, for changes
//!   no event announces (default 0: never).
//!
//! Built for `wasm32-wasip1` as a plain core module, never componentized.

use std::cell::{Cell, RefCell};

use crate::terminal_extensions::statusline::host;

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

/// The command the manifest declares, and the placeholder it takes the directory in.
const STATUS_COMMAND: &str = "status";
const CWD_PLACEHOLDER: &str = "cwd";

/// The key of the setting that holds the refresh interval.
const REFRESH_KEY: &str = "refresh-ms";

/// What follows the branch when the working tree has changes.
const DIRTY_MARKER: &str = "*";

/// How many hex digits of the commit a detached head shows.
const SHORT_COMMIT_LENGTH: usize = 7;

/// Whether the pane is focused, as far as this extension follows it.
#[derive(Clone, Copy, PartialEq)]
enum Focus {
    Unfocused,
    Focused,
}

thread_local! {
    /// The working directory git is asked about. A guest instance is single-threaded.
    static CWD: RefCell<String> = const { RefCell::new(String::new()) };

    /// Whether the pane is focused: the periodic refresh runs only while it is.
    static FOCUS: Cell<Focus> = const { Cell::new(Focus::Unfocused) };

    /// The refresh interval in milliseconds; 0 for none.
    static REFRESH_MS: Cell<u32> = const { Cell::new(0) };

    /// The failure logged last, so that the same one is not logged on every refresh.
    static LOGGED: Cell<Option<host::CommandError>> = const { Cell::new(None) };

    /// What `render` writes.
    static SHOWN: RefCell<String> = const { RefCell::new(String::new()) };
}

/// @return What to show for the output of `git status --porcelain=v2 --branch`: the branch, or the short
///         commit of a detached head, marked when anything is changed; empty when @p status names no head.
fn describe_status(status: &str) -> String {
    let mut head = None;
    let mut commit = None;
    let mut dirty = false;
    for line in status.lines() {
        if let Some(name) = line.strip_prefix("# branch.head ") {
            head = Some(name.trim());
        } else if let Some(oid) = line.strip_prefix("# branch.oid ") {
            commit = Some(oid.trim());
        } else if !line.is_empty() && !line.starts_with('#') {
            dirty = true;
        }
    }
    let name = match head {
        Some("(detached)") => commit.map(|oid| oid.get(..SHORT_COMMIT_LENGTH).unwrap_or(oid)),
        other => other,
    };
    match name {
        Some(name) if dirty => format!("{name}{DIRTY_MARKER}"),
        Some(name) => name.to_string(),
        None => String::new(),
    }
}

/// @return @p error's name in the world.
fn error_name(error: host::CommandError) -> &'static str {
    match error {
        host::CommandError::NotDeclared => "not-declared",
        host::CommandError::Pending => "pending",
        host::CommandError::Denied => "denied",
        host::CommandError::Failed => "failed",
        host::CommandError::TimedOut => "timed-out",
    }
}

/// Logs @p error, unless it is the one logged last. Waiting for the user's answer is no failure.
fn log_failure(error: host::CommandError, cwd: &str) {
    if error == host::CommandError::Pending || LOGGED.get() == Some(error) {
        return;
    }
    LOGGED.set(Some(error));
    host::log(
        host::LogLevel::Warn,
        &format!("git status in {cwd}: {}", error_name(error)),
    );
}

/// Asks git about the working directory, and remembers the answer for `render`.
/// @return Whether what is shown changed.
fn refresh() -> bool {
    let cwd = CWD.with(|cwd| cwd.borrow().clone());
    let now = if cwd.is_empty() {
        String::new()
    } else {
        let args = [(CWD_PLACEHOLDER.to_string(), cwd.clone())];
        match host::run_command(STATUS_COMMAND, &args) {
            Ok(output) => {
                LOGGED.set(None);
                describe_status(&String::from_utf8_lossy(&output.stdout))
            }
            Err(error) => {
                log_failure(error, &cwd);
                String::new()
            }
        }
    };
    SHOWN.with(|shown| {
        let changed = *shown.borrow() != now;
        *shown.borrow_mut() = now;
        changed
    })
}

/// Asks for the next periodic refresh, if one is configured and the pane is focused.
fn schedule() {
    let interval = REFRESH_MS.get();
    if interval > 0 && FOCUS.get() == Focus::Focused {
        host::request_frame(interval);
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

struct Git;

impl Guest for Git {
    fn load(config: Vec<(String, String)>) -> EventKinds {
        let interval = config
            .iter()
            .find(|(key, _)| key == REFRESH_KEY)
            .and_then(|(_, value)| value.trim().parse().ok());
        if let Some(interval) = interval {
            REFRESH_MS.set(interval);
        }
        // What the pane is in already, if it is known: the first prompt shows the branch without waiting
        // for the next change of directory.
        let state = host::state();
        CWD.with(|cwd| *cwd.borrow_mut() = state.cwd);
        FOCUS.set(if state.focused {
            Focus::Focused
        } else {
            Focus::Unfocused
        });
        refresh();
        schedule();
        EventKinds::CWD | EventKinds::FOREGROUND_PROCESS | EventKinds::FOCUS
    }

    fn update(ev: Event) -> RenderRequest {
        match ev {
            Event::Cwd(cwd) => {
                CWD.with(|current| *current.borrow_mut() = cwd);
                render_if(refresh())
            }
            Event::ForegroundProcess(_) => render_if(refresh()),
            Event::Focus(true) => {
                let was = FOCUS.replace(Focus::Focused);
                schedule();
                // Gaining the focus refreshes: whatever changed meanwhile, here or in another pane.
                render_if(was == Focus::Unfocused && refresh())
            }
            Event::Focus(false) => {
                FOCUS.set(Focus::Unfocused);
                RenderRequest::None
            }
            Event::Timer if FOCUS.get() == Focus::Focused => {
                schedule();
                render_if(refresh())
            }
            _ => RenderRequest::None,
        }
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        SHOWN.with(|shown| shown.borrow().clone().into_bytes())
    }
}

export!(Git);

// The one command the host may run on this extension's behalf.
manifest::declare_manifest!(
    "name = git\nversion = 1\ncommand status = git --no-optional-locks -C {cwd} status --porcelain=v2 --branch\n"
);
