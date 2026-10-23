// SPDX-License-Identifier: Apache-2.0
//! Bundled extension for the `terminal-extensions:statusline` world: the local time of day, as `HH:MM`.
//!
//! The reference "hello world" for extension authors. The time comes from the host's `state` import,
//! the only clock an extension has: `unix-time-ms`, read in the time zone `utc-offset-minutes` names,
//! which the host reports anew each time, daylight saving included. The extension asks for a `timer`
//! event at the next minute boundary, so it wakes once a minute, and `update` answers `render` only when
//! the minute shown has changed.
//!
//! Built for `wasm32-wasip1` as a plain core module, never componentized.

use std::cell::RefCell;

use crate::terminal_extensions::statusline::host;

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

const MINUTES_PER_DAY: i64 = 24 * 60;
const MILLISECONDS_PER_MINUTE: u64 = 60 * 1000;

thread_local! {
    /// What `render` writes. A guest instance is single-threaded.
    static SHOWN: RefCell<String> = const { RefCell::new(String::new()) };
}

/// @return The local time of day at @p unix_time_ms, @p offset_minutes ahead of UTC, as `HH:MM`.
fn time_of_day(unix_time_ms: u64, offset_minutes: i32) -> String {
    // Minutes since the epoch fit an i64 for any u64 of milliseconds.
    let minutes = (unix_time_ms / MILLISECONDS_PER_MINUTE) as i64 + i64::from(offset_minutes);
    let minute_of_day = minutes.rem_euclid(MINUTES_PER_DAY);
    format!("{:02}:{:02}", minute_of_day / 60, minute_of_day % 60)
}

/// @return The milliseconds from @p unix_time_ms to the next minute boundary; a whole minute on one.
fn until_next_minute(unix_time_ms: u64) -> u32 {
    // Less than a minute, so it fits a u32.
    (MILLISECONDS_PER_MINUTE - unix_time_ms % MILLISECONDS_PER_MINUTE) as u32
}

/// Reads the time, remembers it for `render`, and asks to be woken when the next minute begins.
/// @return Whether the time shown changed.
fn refresh() -> bool {
    let state = host::state();
    let now = time_of_day(state.unix_time_ms, state.utc_offset_minutes);
    host::request_frame(until_next_minute(state.unix_time_ms));
    SHOWN.with(|shown| {
        let changed = *shown.borrow() != now;
        *shown.borrow_mut() = now;
        changed
    })
}

struct Clock;

impl Guest for Clock {
    fn load(_config: Vec<(String, String)>) -> EventKinds {
        refresh();
        EventKinds::empty()
    }

    fn update(ev: Event) -> RenderRequest {
        match ev {
            Event::Timer if refresh() => RenderRequest::Render,
            _ => RenderRequest::None,
        }
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        SHOWN.with(|shown| shown.borrow().clone().into_bytes())
    }
}

export!(Clock);

manifest::declare_manifest!("name = clock\nversion = 1\n");
