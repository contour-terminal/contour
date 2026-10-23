// SPDX-License-Identifier: Apache-2.0
//! Test guest for the `terminal-extensions:statusline` world (`vtextension_test`).
//!
//! `load` reads the `text` setting; `render` writes it. A `reply` event replaces the output with the
//! reply's bytes as lowercase hex, which lets a test see what the host delivered. Two more settings
//! let a test exercise the rest of the contract: `subscribe` (a comma-separated list of event kind
//! names) is what `load` returns, and `log` is a message `load` passes to the host's `log` import.
//! Two more make the guest describe what it receives, so that a test can check every type the host
//! lowers: `events = describe` renders each event as text (see [`describe_event`]) and asks for a
//! render, and `state = describe` makes `load` read the host's `state` and render it as text (see
//! [`describe_state`]). `trap = unfocused` makes `render` trap while the pane is unfocused, so that a test can
//! fault an instance and let the next one, after the host restarts it, succeed.
//! Built for `wasm32-wasip1` as a plain core module, never componentized.

use std::cell::RefCell;
use std::fmt::Write;

use crate::terminal_extensions::statusline::host;
use crate::terminal_extensions::statusline::types::{
    FontMetrics, InputMode, Progress, ProgressState, Tab,
};

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

/// The key of the setting that holds the text to draw.
const TEXT_KEY: &str = "text";

/// The key of the setting that lists the event kinds to subscribe to.
const SUBSCRIBE_KEY: &str = "subscribe";

/// The key of the setting that holds a message to send to the host's log.
const LOG_KEY: &str = "log";

/// The key of the setting that, set to [`DESCRIBE`], makes every event render as its description.
const EVENTS_KEY: &str = "events";

/// The key of the setting that, set to [`DESCRIBE`], makes `load` render the host's state.
const STATE_KEY: &str = "state";

/// The key of the setting that, set to [`UNFOCUSED`], makes `render` trap while the pane is unfocused.
const TRAP_KEY: &str = "trap";

/// The value of [`TRAP_KEY`] that turns trapping on.
const UNFOCUSED: &str = "unfocused";

/// The value of [`EVENTS_KEY`] and [`STATE_KEY`] that turns description on.
const DESCRIBE: &str = "describe";

/// The event kind names a `subscribe` setting may list, and the flag each stands for.
const EVENT_KINDS: [(&str, EventKinds); 9] = [
    ("title", EventKinds::TITLE),
    ("cwd", EventKinds::CWD),
    ("foreground-process", EventKinds::FOREGROUND_PROCESS),
    ("input-mode", EventKinds::INPUT_MODE),
    ("tabs", EventKinds::TABS),
    ("user-var", EventKinds::USER_VAR),
    ("progress", EventKinds::PROGRESS),
    ("focus", EventKinds::FOCUS),
    ("font", EventKinds::FONT),
];

/// @return The flags for the names in the comma-separated @p names; unknown names are ignored.
fn parse_event_kinds(names: &str) -> EventKinds {
    names
        .split(',')
        .map(str::trim)
        .filter_map(|name| EVENT_KINDS.iter().find(|(known, _)| *known == name))
        .fold(EventKinds::empty(), |kinds, (_, flag)| kinds | *flag)
}

/// @return The WIT name of @p mode.
fn input_mode_name(mode: InputMode) -> &'static str {
    match mode {
        InputMode::Insert => "insert",
        InputMode::Normal => "normal",
        InputMode::Visual => "visual",
        InputMode::VisualLine => "visual-line",
        InputMode::VisualBlock => "visual-block",
        InputMode::Hint => "hint",
    }
}

/// @return @p progress as `<state>/<percent>`, with the state's WIT name.
fn describe_progress(progress: Progress) -> String {
    let state = match progress.state {
        ProgressState::Hidden => "hidden",
        ProgressState::Normal => "normal",
        ProgressState::Error => "error",
        ProgressState::Indeterminate => "indeterminate",
        ProgressState::Warning => "warning",
    };
    format!("{state}/{}", progress.percent)
}

/// @return @p font as `<size>pt <width>x<height>`.
fn describe_font(font: FontMetrics) -> String {
    format!(
        "{}pt {}x{}",
        font.size_pt, font.cell_width_px, font.cell_height_px
    )
}

/// @return @p tabs as their titles joined by `|`, the active one marked with a trailing `*`.
fn describe_tabs(tabs: &[Tab]) -> String {
    tabs.iter()
        .map(|tab| format!("{}{}", tab.title, if tab.active { "*" } else { "" }))
        .collect::<Vec<_>>()
        .join("|")
}

/// @return @p bytes as lowercase hex.
fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        // Writing to a String cannot fail.
        let _ = write!(text, "{byte:02x}");
    }
    text
}

/// @return @p ev as `<case>:<payload>`, with the case's WIT name.
fn describe_event(ev: &Event) -> String {
    match ev {
        Event::Title(title) => format!("title:{title}"),
        Event::Cwd(cwd) => format!("cwd:{cwd}"),
        Event::ForegroundProcess(name) => format!("foreground-process:{name}"),
        Event::InputMode(mode) => format!("input-mode:{}", input_mode_name(*mode)),
        Event::Tabs(tabs) => format!("tabs:{}", describe_tabs(tabs)),
        Event::UserVar((name, value)) => format!("user-var:{name}={value}"),
        Event::Progress(progress) => format!("progress:{}", describe_progress(*progress)),
        Event::Focus(focused) => format!("focus:{focused}"),
        Event::FontChanged(font) => format!("font-changed:{}", describe_font(*font)),
        Event::Resized(size) => format!("resized:{}x{}", size.cols, size.rows),
        Event::Timer => "timer".to_string(),
        Event::Reply(bytes) => format!("reply:{}", hex(bytes)),
    }
}

/// @return Every field of @p state, `;`-separated, in declaration order.
fn describe_state(state: &host::StateSnapshot) -> String {
    let user_vars = state
        .user_vars
        .iter()
        .map(|(name, value)| format!("{name}={value}"))
        .collect::<Vec<_>>()
        .join(",");
    format!(
        "{};{};{};{};{};{};{};{};{}x{};{};{};{}",
        state.title,
        state.cwd,
        state.foreground_process,
        input_mode_name(state.input_mode),
        describe_tabs(&state.tabs),
        user_vars,
        describe_progress(state.progress),
        state.focused,
        state.size.cols,
        state.size.rows,
        state.unix_time_ms,
        state.utc_offset_minutes,
        describe_font(state.font)
    )
}

struct Echo;

thread_local! {
    /// What `render` writes. A guest instance is single-threaded, so this is the instance's state.
    static OUTPUT: RefCell<String> = const { RefCell::new(String::new()) };

    /// Whether `update` describes every event (the `events` setting).
    static DESCRIBE_EVENTS: RefCell<bool> = const { RefCell::new(false) };

    /// Whether `render` traps while the pane is unfocused (the `trap` setting).
    static TRAP_UNFOCUSED: RefCell<bool> = const { RefCell::new(false) };
}

fn set_output(text: String) {
    OUTPUT.with(|output| *output.borrow_mut() = text);
}

impl Guest for Echo {
    fn load(config: Vec<(String, String)>) -> EventKinds {
        let setting = |wanted: &str| {
            config
                .iter()
                .find_map(|(key, value)| (key == wanted).then_some(value.as_str()))
        };
        if let Some(message) = setting(LOG_KEY) {
            host::log(host::LogLevel::Info, message);
        }
        set_output(setting(TEXT_KEY).unwrap_or_default().to_string());
        DESCRIBE_EVENTS
            .with(|describe| *describe.borrow_mut() = setting(EVENTS_KEY) == Some(DESCRIBE));
        TRAP_UNFOCUSED.with(|trap| *trap.borrow_mut() = setting(TRAP_KEY) == Some(UNFOCUSED));
        if setting(STATE_KEY) == Some(DESCRIBE) {
            set_output(describe_state(&host::state()));
        }
        setting(SUBSCRIBE_KEY).map_or(EventKinds::empty(), parse_event_kinds)
    }

    fn update(ev: Event) -> RenderRequest {
        if DESCRIBE_EVENTS.with(|describe| *describe.borrow()) {
            set_output(describe_event(&ev));
            return RenderRequest::Render;
        }
        match ev {
            Event::Reply(bytes) => {
                set_output(hex(&bytes));
                RenderRequest::Render
            }
            _ => RenderRequest::None,
        }
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        if TRAP_UNFOCUSED.with(|trap| *trap.borrow()) && !host::state().focused {
            core::arch::wasm32::unreachable();
        }
        OUTPUT.with(|output| output.borrow().clone().into_bytes())
    }
}

export!(Echo);

// The manifest `parseManifest` reads back in the host's tests.
manifest::declare_manifest!(
    "name = echo\nversion = 1\ncommand greet = echo hello {name}\ncommand plain = true\n"
);
