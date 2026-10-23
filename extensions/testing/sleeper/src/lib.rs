// SPDX-License-Identifier: Apache-2.0
//! Test guest for the `terminal-extensions:statusline` world that sleeps (`vtextension_test`).
//!
//! `render` calls `std::thread::sleep`, which compiles to WASI's `poll_oneoff`: a wait the host's
//! watchdog cannot interrupt. The host refuses the module at load, so `render` never runs. This is a
//! crate of its own, not a mode of the `hostile` guest, because the import alone makes a module
//! unloadable. Built for `wasm32-wasip1` as a plain core module, never componentized.

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

struct Sleeper;

impl Guest for Sleeper {
    fn load(_config: Vec<(String, String)>) -> EventKinds {
        EventKinds::empty()
    }

    fn update(_ev: Event) -> RenderRequest {
        RenderRequest::Render
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        std::thread::sleep(std::time::Duration::from_secs(3600));
        b"rested".to_vec()
    }
}

export!(Sleeper);
