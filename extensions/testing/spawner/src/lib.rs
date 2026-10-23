// SPDX-License-Identifier: Apache-2.0
//! Test guest for the `terminal-extensions:statusline` world that runs a command (`vtextension_test`).
//!
//! `render` calls `run-command` with the `command` setting (default `echo`) and the `args` setting
//! (default `word=hello`): comma-separated `name=value` pairs, each split at its first `=`. It renders
//! the outcome: `ok:<exit code>:<stdout>:<stderr>`
//! or `err:<command-error>`. The manifest declares the one command `echo = echo {word}`.
//! Built for `wasm32-wasip1` as a plain core module, never componentized.

use std::cell::RefCell;

use crate::terminal_extensions::statusline::host;

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

thread_local! {
    /// The command to run and its arguments. A guest instance is single-threaded.
    static CALL: RefCell<(String, Vec<(String, String)>)> = const { RefCell::new((String::new(), Vec::new())) };
}

struct Spawner;

impl Guest for Spawner {
    fn load(config: Vec<(String, String)>) -> EventKinds {
        let setting = |wanted: &str| {
            config
                .iter()
                .find_map(|(key, value)| (key == wanted).then_some(value.clone()))
        };
        let command = setting("command").unwrap_or_else(|| "echo".to_string());
        let args = setting("args")
            .unwrap_or_else(|| "word=hello".to_string())
            .split(',')
            .filter(|arg| !arg.is_empty())
            .map(|arg| match arg.split_once('=') {
                Some((name, value)) => (name.to_string(), value.to_string()),
                None => (arg.to_string(), String::new()),
            })
            .collect();
        CALL.with(|call| *call.borrow_mut() = (command, args));
        EventKinds::empty()
    }

    fn update(_ev: Event) -> RenderRequest {
        RenderRequest::Render
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        let (command, args) = CALL.with(|call| call.borrow().clone());
        let outcome = match host::run_command(&command, &args) {
            Ok(output) => format!(
                "ok:{}:{}:{}",
                output.exit_code,
                String::from_utf8_lossy(&output.stdout),
                String::from_utf8_lossy(&output.stderr)
            ),
            Err(error) => format!(
                "err:{}",
                match error {
                    host::CommandError::NotDeclared => "not-declared",
                    host::CommandError::Pending => "pending",
                    host::CommandError::Denied => "denied",
                    host::CommandError::Failed => "failed",
                    host::CommandError::TimedOut => "timed-out",
                }
            ),
        };
        outcome.into_bytes()
    }
}

export!(Spawner);

// The one command the host may run on this guest's behalf.
manifest::declare_manifest!("name = spawner\nversion = 1\ncommand echo = echo {word}\n");
