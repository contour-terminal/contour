// SPDX-License-Identifier: Apache-2.0
//! Embeds an extension's manifest in the module it is compiled into.
//!
//! The host reads the manifest from the custom section named [`SECTION`], before it runs any of the
//! module's code. The format is documented in `wit/statusline/README.md`.

/// The name of the custom section that carries the manifest.
pub const SECTION: &str = "terminal-extension-manifest";

/// Emits `$text` as the module's manifest.
///
/// `$text` must be a constant `&str`, such as a string literal or `include_str!(...)`. Call it once,
/// at the top level of the guest crate:
///
/// ```ignore
/// manifest::declare_manifest!("name = git\nversion = 1\ncommand status = git -C {cwd} status\n");
/// ```
///
/// **Only the guest crate itself may call this.** The linker concatenates the custom sections of
/// the same name that every crate in the dependency graph contributes, and the host refuses a module
/// whose manifest section appears twice. A dependency crate that calls `declare_manifest!` therefore
/// makes the whole module unloadable.
#[macro_export]
macro_rules! declare_manifest {
    ($text:expr) => {
        const _: () = {
            const BYTES: &[u8] = ($text).as_bytes();

            const fn copy() -> [u8; BYTES.len()] {
                let mut out = [0u8; BYTES.len()];
                let mut index = 0;
                while index < BYTES.len() {
                    out[index] = BYTES[index];
                    index += 1;
                }
                out
            }

            #[used]
            #[link_section = "terminal-extension-manifest"]
            static MANIFEST: [u8; BYTES.len()] = copy();
        };
    };
}
