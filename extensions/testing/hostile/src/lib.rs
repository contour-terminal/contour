// SPDX-License-Identifier: Apache-2.0
//! Test guest for the `terminal-extensions:statusline` world that misbehaves on purpose
//! (`vtextension_test`).
//!
//! The `mode` setting picks the misbehaviour; `render` carries it out, except for `spin-load`, which
//! `load` carries out. Without a `mode` the guest behaves and renders "calm".
//!
//! - `spin`: never returns.
//! - `spin-load`: `load` never returns.
//! - `trap`: executes `unreachable`.
//! - `alloc`: allocates until the memory cap stops it.
//! - `frame-flood`: calls `request-frame` [`FRAME_FLOOD`] times, then renders "flooded".
//! - `huge-output`: renders [`HUGE_OUTPUT`] bytes.
//! - `print`: writes to its standard output and error in `load` and `render`, then renders "printed".
//!
//! The module carries no manifest, so it also stands for an extension that declares no commands.
//! Built for `wasm32-wasip1` as a plain core module, never componentized.

use std::cell::RefCell;

use crate::terminal_extensions::statusline::host;

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

/// How many times `frame-flood` calls `request-frame` in one `render`.
const FRAME_FLOOD: u32 = 1000;

/// How many bytes `huge-output` renders: more than any sensible host limit (1 MiB by default).
const HUGE_OUTPUT: usize = 2 * 1024 * 1024;

thread_local! {
    /// The `mode` setting. A guest instance is single-threaded, so this is the instance's state.
    static MODE: RefCell<String> = const { RefCell::new(String::new()) };
}

fn mode() -> String {
    MODE.with(|mode| mode.borrow().clone())
}

/// Spins forever without the optimizer noticing that nothing happens.
fn spin() -> ! {
    let mut counter: u64 = 0;
    loop {
        counter = core::hint::black_box(counter.wrapping_add(1));
    }
}

struct Hostile;

impl Guest for Hostile {
    fn load(config: Vec<(String, String)>) -> EventKinds {
        let mode = config
            .into_iter()
            .find_map(|(key, value)| (key == "mode").then_some(value))
            .unwrap_or_default();
        match mode.as_str() {
            "spin-load" => spin(),
            "print" => {
                println!("hostile: printed from load");
                eprintln!("hostile: printed from load");
            }
            _ => {}
        }
        MODE.with(|stored| *stored.borrow_mut() = mode);
        EventKinds::empty()
    }

    fn update(_ev: Event) -> RenderRequest {
        RenderRequest::Render
    }

    fn render(_cols: u32, _rows: u32) -> Vec<u8> {
        match mode().as_str() {
            "spin" => spin(),
            "trap" => core::arch::wasm32::unreachable(),
            "alloc" => {
                let mut hoard: Vec<Vec<u8>> = Vec::new();
                loop {
                    hoard.push(core::hint::black_box(vec![1u8; 1024 * 1024]));
                }
            }
            "frame-flood" => {
                for _ in 0..FRAME_FLOOD {
                    host::request_frame(0);
                }
                b"flooded".to_vec()
            }
            "huge-output" => vec![b'x'; HUGE_OUTPUT],
            "print" => {
                println!("hostile: printed from render");
                eprintln!("hostile: printed from render");
                b"printed".to_vec()
            }
            _ => b"calm".to_vec(),
        }
    }
}

export!(Hostile);
