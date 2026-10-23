// SPDX-License-Identifier: Apache-2.0
//! Bundled extension for the `terminal-extensions:statusline` world: a badge for the state a coding agent
//! reports in a user variable, and beside it the progress it reports.
//!
//! An agent (or a hook it runs) sets the user variable `agent_status` to `working`, `waiting` or `done`,
//! for example with `printf '\e]1337;SetUserVar=agent_status=%s\a' "$(printf waiting | base64)"`. Each
//! state is drawn in a colour of its own (see [`STATES`]); any other value in a neutral one, and an empty
//! value hides the badge. A program that reports progress (OSC 9;4) gets a segment beside it: the
//! percentage, or an ellipsis while the extent is unknown, coloured by how it goes (see [`PROGRESS`]).
//! Both are read from the host's `state` at `load`, so what was set before the extension started shows
//! at once.
//!
//! A user variable holds whatever bytes a program in the pane chose. The value is shown as text only:
//! control characters (C0, DEL and C1, so no escape sequence) are dropped, and it is cut at
//! [`MAX_VALUE_CHARS`] characters.
//!
//! Settings:
//! - `variable`: the user variable to follow (default `agent_status`).
//!
//! Built for `wasm32-wasip1` as a plain core module, never componentized.

use std::cell::{Cell, RefCell};

use crate::terminal_extensions::statusline::host;
use crate::terminal_extensions::statusline::types::{Progress, ProgressState};

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

/// The key of the setting that names the user variable.
const VARIABLE_KEY: &str = "variable";
const DEFAULT_VARIABLE: &str = "agent_status";

/// The most characters of a value the badge shows.
const MAX_VALUE_CHARS: usize = 32;

/// A 24-bit colour.
#[derive(Clone, Copy)]
struct Rgb(u8, u8, u8);

/// How a segment looks.
struct Style {
    symbol: &'static str,
    foreground: Rgb,
    background: Rgb,
}

/// Each state an agent reports, and how its badge looks.
const STATES: [(&str, Style); 3] = [
    (
        "working",
        Style {
            symbol: "●",
            foreground: Rgb(0x00, 0x00, 0x00),
            background: Rgb(0xE5, 0xA5, 0x0A),
        },
    ),
    (
        "waiting",
        Style {
            symbol: "?",
            foreground: Rgb(0xFF, 0xFF, 0xFF),
            background: Rgb(0xD7, 0x3A, 0x49),
        },
    ),
    (
        "done",
        Style {
            symbol: "✓",
            foreground: Rgb(0x00, 0x00, 0x00),
            background: Rgb(0x2E, 0xA0, 0x43),
        },
    ),
];

/// How a state not in [`STATES`] looks.
const OTHER: Style = Style {
    symbol: "·",
    foreground: Rgb(0xFF, 0xFF, 0xFF),
    background: Rgb(0x6E, 0x76, 0x81),
};

/// How the progress segment looks in each state that shows one. Hidden shows none.
const PROGRESS: [(ProgressState, Style); 4] = [
    (
        ProgressState::Normal,
        Style {
            symbol: "▶",
            foreground: Rgb(0xFF, 0xFF, 0xFF),
            background: Rgb(0x1F, 0x6F, 0xEB),
        },
    ),
    (
        ProgressState::Indeterminate,
        Style {
            symbol: "▶",
            foreground: Rgb(0xFF, 0xFF, 0xFF),
            background: Rgb(0x1F, 0x6F, 0xEB),
        },
    ),
    (
        ProgressState::Warning,
        Style {
            symbol: "!",
            foreground: Rgb(0x00, 0x00, 0x00),
            background: Rgb(0xE5, 0xA5, 0x0A),
        },
    ),
    (
        ProgressState::Error,
        Style {
            symbol: "✗",
            foreground: Rgb(0xFF, 0xFF, 0xFF),
            background: Rgb(0xD7, 0x3A, 0x49),
        },
    ),
];

thread_local! {
    /// The user variable followed. A guest instance is single-threaded.
    static VARIABLE: RefCell<String> = RefCell::new(DEFAULT_VARIABLE.to_string());

    /// Its value, made displayable: the state shown.
    static STATE: RefCell<String> = const { RefCell::new(String::new()) };

    /// The progress shown.
    static PROGRESS_SHOWN: Cell<Progress> = const {
        Cell::new(Progress { state: ProgressState::Hidden, percent: 0 })
    };
}

/// @return @p value as text a badge may show: control characters dropped, cut at [`MAX_VALUE_CHARS`].
fn displayable(value: &str) -> String {
    value
        .chars()
        .filter(|c| !c.is_control())
        .take(MAX_VALUE_CHARS)
        .collect()
}

/// @return @p text on @p style's background, its symbol first, with a blank either side.
fn segment(style: &Style, text: &str) -> String {
    let Rgb(fr, fg, fb) = style.foreground;
    let Rgb(br, bg, bb) = style.background;
    format!(
        "\x1b[1;38;2;{fr};{fg};{fb};48;2;{br};{bg};{bb}m {} {text} \x1b[m",
        style.symbol
    )
}

/// @return The badge for @p state; nothing for an empty one.
fn badge(state: &str) -> String {
    if state.is_empty() {
        return String::new();
    }
    let style = STATES
        .iter()
        .find(|(name, _)| *name == state)
        .map_or(&OTHER, |(_, style)| style);
    segment(style, state)
}

/// @return The progress segment for @p progress; nothing while it is hidden.
fn progress_segment(progress: Progress) -> String {
    let Some((_, style)) = PROGRESS.iter().find(|(state, _)| *state == progress.state) else {
        return String::new();
    };
    let text = match progress.state {
        ProgressState::Indeterminate => "…".to_string(),
        _ => format!("{}%", progress.percent.min(100)),
    };
    segment(style, &text)
}

/// Shows the variable's value @p value.
/// @return Whether that changed what is shown.
fn show_state(value: &str) -> bool {
    let state = displayable(value);
    STATE.with(|shown| {
        let changed = *shown.borrow() != state;
        *shown.borrow_mut() = state;
        changed
    })
}

/// Shows @p progress.
/// @return Whether that changed what is shown.
fn show_progress(progress: Progress) -> bool {
    let previous = PROGRESS_SHOWN.replace(progress);
    previous.state != progress.state || previous.percent != progress.percent
}

/// @return `render` if @p changed, else `none`.
fn render_if(changed: bool) -> RenderRequest {
    if changed {
        RenderRequest::Render
    } else {
        RenderRequest::None
    }
}

struct AgentStatus;

impl Guest for AgentStatus {
    fn load(config: Vec<(String, String)>) -> EventKinds {
        if let Some((_, name)) = config.into_iter().find(|(key, _)| key == VARIABLE_KEY) {
            VARIABLE.with(|variable| *variable.borrow_mut() = name);
        }
        let variable = VARIABLE.with(|variable| variable.borrow().clone());
        let state = host::state();
        if let Some((_, value)) = state.user_vars.iter().find(|(name, _)| *name == variable) {
            show_state(value);
        }
        show_progress(state.progress);
        EventKinds::USER_VAR | EventKinds::PROGRESS
    }

    fn update(ev: Event) -> RenderRequest {
        match ev {
            Event::UserVar((name, value))
                if VARIABLE.with(|variable| *variable.borrow() == name) =>
            {
                render_if(show_state(&value))
            }
            Event::Progress(progress) => render_if(show_progress(progress)),
            _ => RenderRequest::None,
        }
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        let badge = STATE.with(|state| badge(&state.borrow()));
        let progress = progress_segment(PROGRESS_SHOWN.get());
        match (badge.is_empty(), progress.is_empty()) {
            (false, false) => format!("{badge} {progress}").into_bytes(),
            _ => (badge + &progress).into_bytes(),
        }
    }
}

export!(AgentStatus);

manifest::declare_manifest!("name = agent-status\nversion = 1\n");
