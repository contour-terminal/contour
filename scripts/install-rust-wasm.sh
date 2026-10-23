#! /bin/sh
# SPDX-License-Identifier: Apache-2.0
set -ex

# Installs what CONTOUR_EXTENSIONS_BUNDLED needs: cargo, rustc and the wasm32-wasip1 standard library,
# through rustup, on the current stable Rust. The bundled status-line extensions and the test guests are
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

# Brings an image's preinstalled stable up to date, too: what it ships may predate what the guests need.
rustup toolchain install stable --profile minimal --target "$WASM_TARGET" --no-self-update
rustup default stable

rustc --version
cargo --version
# The same probe cmake/ContourCargo.cmake makes, so that a broken install fails here and says so.
ls "$(rustc --print target-libdir --target "$WASM_TARGET")"/libstd-*.rlib
