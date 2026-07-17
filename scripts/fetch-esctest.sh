#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Fetches esctest2, the conformance suite Contour is measured against.
#
# esctest is NOT vendored, and must not be: it is GPL-2.0 while Contour is Apache-2.0, so it is
# cloned into a build directory and invoked as an external program. Nothing of it is compiled or
# linked into Contour.
#
# Usage: scripts/fetch-esctest.sh [target-directory]
#
# The conformance gate skips (rather than fails) when esctest is absent, so running this is optional
# — but without it, roughly two thirds of Contour's automated VT coverage is not measured.

set -euo pipefail

# Contour's fork of esctest2, on the branch that adds the "contour" terminal profile: it selects
# xterm's expected values (Contour is xterm-compatible) but is a distinct terminal for knownBug(),
# where Contour is frequently more correct than xterm. Upstream is ThomasDickey/esctest2; the fork
# tracks it and only adds that awareness. Override with the environment for a plain upstream run:
#   ESCTEST_REPOSITORY=https://github.com/ThomasDickey/esctest2.git ESCTEST_BRANCH=master
REPOSITORY="${ESCTEST_REPOSITORY:-https://github.com/contour-terminal/esctest2.git}"
BRANCH="${ESCTEST_BRANCH:-contour-aware}"
TARGET="${1:-$(dirname "$0")/../out/esctest2}"

if ! command -v git >/dev/null 2>&1; then
    echo "error: git is required to fetch esctest." >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required to run esctest." >&2
    exit 1
fi

if [ -d "$TARGET/.git" ]; then
    echo "Updating esctest ($BRANCH) in $TARGET ..."
    # $REPOSITORY is the single source of truth for where esctest comes from, so point origin at it
    # rather than trusting whatever the existing checkout happens to carry. A tree cloned by hand may
    # well have origin on upstream (which has no "contour-aware" branch), and an ESCTEST_REPOSITORY
    # override must behave the same here as it does on the clone path below -- silently fetching from
    # the previous remote is the worst outcome, because it looks like it worked.
    git -C "$TARGET" remote set-url origin "$REPOSITORY" 2>/dev/null \
        || git -C "$TARGET" remote add origin "$REPOSITORY"
    git -C "$TARGET" fetch --depth 1 origin "$BRANCH"
    git -C "$TARGET" checkout -B "$BRANCH" FETCH_HEAD
else
    echo "Cloning esctest ($BRANCH) into $TARGET ..."
    git clone --depth 1 --branch "$BRANCH" "$REPOSITORY" "$TARGET"
fi

# The suite's entry point lives one level down; that is the directory the harness runs it from.
ESCTEST_DIR="$(cd "$TARGET/esctest" && pwd)"

echo
echo "esctest is ready."
echo
echo "  Run it against Contour with:"
echo "    ctest --preset=clang-asan -L conformance"
echo
echo "  or directly:"
echo "    vtconformance-run run --suite esctest --suite-dir '$ESCTEST_DIR'"
echo
echo "  License: GPL-2.0. Used as an external tool only; never linked into Contour."
