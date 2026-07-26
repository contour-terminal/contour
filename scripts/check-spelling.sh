#! /usr/bin/env bash
# Spell-checks the repository with `typos`, using the configuration in `_typos.toml`.
#
# This script is the single source of truth for the spell gate: both `ctest -L lint` and the CI
# job run *this file*, so a local run and a CI run cannot disagree -- they are the same command,
# at the same pinned tool version, against the same config. That property is the whole point;
# do not add a second invocation path.
#
# typos is fetched on demand into the build tree if not already on PATH, so this needs no system
# install. If it cannot be obtained (offline), the check SKIPS rather than fails, matching the
# other optional check-*.sh scripts. CI sets CHECK_SPELLING_REQUIRE_TOOL=1 to turn that skip into
# a hard failure, so a failed download can never show up as a green gate.
set -euo pipefail

# The pinned version. Because CI runs this same script, this single line keeps local and CI in
# lockstep -- there is no second place to update.
TYPOS_VERSION="1.48.0"

SOURCE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SOURCE_DIR"

CACHE_DIR="${TYPOS_CACHE_DIR:-$SOURCE_DIR/out/.tools}"

# Reports that the tool is unavailable, then either skips or fails depending on the caller.
# CI wants a red build; a developer offline in a fresh clone wants to get on with their day.
unavailable() {
    if [ "${CHECK_SPELLING_REQUIRE_TOOL:-0}" = "1" ]; then
        echo "typos v$TYPOS_VERSION is required but could not be obtained: $1" >&2
        exit 1
    fi
    echo "typos not available ($1); skipping spell check." >&2
    exit 0
}

# Maps uname to the release asset triple published by crate-ci/typos.
typos_asset() {
    local kernel machine
    kernel="$(uname -s)"
    machine="$(uname -m)"
    case "$kernel/$machine" in
        # The Linux builds are static musl binaries, so they carry no runtime dependency.
        Linux/x86_64)          echo "typos-v$TYPOS_VERSION-x86_64-unknown-linux-musl.tar.gz" ;;
        Linux/aarch64|Linux/arm64) echo "typos-v$TYPOS_VERSION-aarch64-unknown-linux-musl.tar.gz" ;;
        Darwin/x86_64)         echo "typos-v$TYPOS_VERSION-x86_64-apple-darwin.tar.gz" ;;
        Darwin/arm64)          echo "typos-v$TYPOS_VERSION-aarch64-apple-darwin.tar.gz" ;;
        *)                     echo "" ;;
    esac
}

# Resolve typos: a PATH copy at the pinned version, then a cached download, then fetch it.
# A PATH copy at a *different* version is deliberately rejected in favour of the pinned one --
# a developer's stray install must not be able to disagree with CI.
TYPOS=""
if command -v typos >/dev/null 2>&1 \
   && [ "$(typos --version 2>/dev/null | awk '{print $2}')" = "$TYPOS_VERSION" ]; then
    TYPOS="$(command -v typos)"
elif [ -x "$CACHE_DIR/typos" ] \
     && [ "$("$CACHE_DIR/typos" --version 2>/dev/null | awk '{print $2}')" = "$TYPOS_VERSION" ]; then
    TYPOS="$CACHE_DIR/typos"
else
    ASSET="$(typos_asset)"
    [ -n "$ASSET" ] || unavailable "no release asset for $(uname -s)/$(uname -m)"

    mkdir -p "$CACHE_DIR"
    URL="https://github.com/crate-ci/typos/releases/download/v$TYPOS_VERSION/$ASSET"
    # Unpack into a scratch directory and move the binary out: the archive stores its members
    # as `./typos`, and naming members explicitly differs between GNU tar and BSD tar.
    UNPACK_DIR="$CACHE_DIR/.typos-unpack"
    rm -rf "$UNPACK_DIR"
    if curl -fsSL "$URL" -o "$CACHE_DIR/$ASSET" 2>/dev/null \
       && mkdir -p "$UNPACK_DIR" \
       && tar -xzf "$CACHE_DIR/$ASSET" -C "$UNPACK_DIR" 2>/dev/null \
       && [ -x "$UNPACK_DIR/typos" ] \
       && mv -f "$UNPACK_DIR/typos" "$CACHE_DIR/typos"; then
        TYPOS="$CACHE_DIR/typos"
    fi
    rm -rf "$CACHE_DIR/$ASSET" "$UNPACK_DIR"

    [ -n "$TYPOS" ] || unavailable "download from $URL failed"
fi

# Every exception in `_typos.toml` must say why it is there, in a comment on its own line. The
# previous spell gate accumulated 1121 unexplained allow-list entries, 86 of which were genuine
# misspellings that the gate then happily accepted -- an unjustified entry is how a word list
# stops being a word list.
#
# The comment is required *inline* rather than merely somewhere above: a preceding comment block
# would let a later entry appended beneath it inherit a justification it was never given.
check_justifications() {
    awk '
        /^\[/           { in_words = ($0 ~ /extend-(identifiers|words)\]$/); next }
        /^[[:space:]]*#/ { next }
        in_words && /=/ && $0 !~ /#/ {
            printf "  %s:%d: %s\n", FILENAME, FNR, $0
            bad++
        }
        END { exit(bad ? 1 : 0) }
    ' _typos.toml
}

if ! JUSTIFY_REPORT="$(check_justifications)"; then
    echo "_typos.toml: these exceptions have no justification comment:" >&2
    echo "$JUSTIFY_REPORT" >&2
    echo "Say why the word is not a typo, or fix the text instead of excusing it." >&2
    exit 1
fi

# No path arguments: typos walks the whole tree under `_typos.toml`, honouring .gitignore.
echo "Running spell check with typos v$TYPOS_VERSION..."
"$TYPOS"
echo "typos: spelling OK ($(grep -cE '^[A-Za-z_]+ = ' _typos.toml) exceptions, all justified)."
