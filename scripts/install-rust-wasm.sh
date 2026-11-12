#! /bin/sh
# SPDX-License-Identifier: Apache-2.0
set -ex

# Installs what CONTOUR_EXTENSIONS_BUNDLED needs: cargo, rustc and the wasm32-wasip1 standard library,
# through rustup, on the Rust release named below. The bundled status-line extensions and the test guests are
# Rust crates built for that target, and a configuration with the option on (the default) fails without
# it rather than building without them.
#
# rustup rather than the distribution's packages, because the guests' wit-bindgen needs Rust 1.88 and
# Ubuntu 24.04 packages 1.75, without a wasm32-wasip1 standard library. GitHub's Linux, macOS and Windows
# images carry rustup already; a machine without it (the FreeBSD VM, a container) gets it from
# rustup.rs. Plain sh, so that the same script serves the BSD VM's sh and Windows' Git Bash.
# install-deps.sh runs it too, as the user who builds, on the systems whose packages do not suffice.
#
# When it installs rustup itself it also appends cargo's bin directory to $GITHUB_PATH, if set, so the
# later steps of a GitHub job find cargo; a caller in one shell session extends PATH itself.

WASM_TARGET="wasm32-wasip1"
# A named release, not "stable": the guests are built by whatever this installs, so "stable" changes what
# every build produces on the day Rust releases. 1.100.0 did exactly that -- the guests it builds trap in
# the WebAssembly runtime. Raise this on purpose, with the extension tests green.
RUST_TOOLCHAIN="1.99.1"
CARGO_BIN="${CARGO_HOME:-$HOME/.cargo}/bin"

if ! command -v rustup >/dev/null 2>&1 && [ -x "$CARGO_BIN/rustup" ]; then
    PATH="$CARGO_BIN:$PATH"
fi

if ! command -v rustup >/dev/null 2>&1; then
    installer="$(mktemp)"
    if command -v curl >/dev/null 2>&1; then
        curl --proto '=https' --tlsv1.2 -sSf -o "$installer" https://sh.rustup.rs
    else
        fetch -o "$installer" https://sh.rustup.rs
    fi
    sh "$installer" -y --profile minimal --default-toolchain none --no-modify-path
    rm -f "$installer"
    PATH="$CARGO_BIN:$PATH"
    if [ -n "${GITHUB_PATH:-}" ]; then
        echo "$CARGO_BIN" >> "$GITHUB_PATH"
    fi
fi

# Whatever Rust an image ships is left alone; the guests are built by this one.
rustup toolchain install "$RUST_TOOLCHAIN" --profile minimal --target "$WASM_TARGET" --no-self-update
rustup default "$RUST_TOOLCHAIN"

rustc --version
cargo --version
# The same probe cmake/ContourCargo.cmake makes, so that a broken install fails here and says so.
ls "$(rustc --print target-libdir --target "$WASM_TARGET")"/libstd-*.rlib
