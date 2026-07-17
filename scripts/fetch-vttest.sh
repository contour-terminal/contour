#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Fetches and builds vttest, the older half of the conformance suite Contour is measured against.
#
# vttest is NOT vendored: it is fetched and built as an external program, and nothing of it is
# compiled or linked into Contour. Unlike esctest that is a convenience rather than a licence
# requirement -- vttest is MIT-licensed -- but the boundary is worth keeping either way.
#
# Usage: scripts/fetch-vttest.sh [target-directory]
#
# vttest is also packaged by most distributions (`apt install vttest`, `dnf install vttest`), and the
# conformance harness is happy with either: it takes whatever is on PATH unless told otherwise with
# `--program`. Running this is optional.

set -euo pipefail

# Contour's fork, on the branch carrying two fixes found by driving vttest headlessly:
#
#   * the `-l` log is line-buffered, so a record that has happened is a record on disk. Upstream
#     leaves it fully buffered, and only `Wait:`/`Done:`, give_up() and menu2() ever flush -- so a
#     vttest that is killed leaves a transcript SHORT of the verdicts it produced, and a short
#     transcript reports fewer failures, which reads as success.
#   * DECCRA goes out with the eight parameters it has. Upstream's format string carries a trailing
#     `;` (esc.c:732), which ECMA-48 6.5.3 makes a ninth, empty one -- legal, but not what DECCRA is,
#     and a terminal that validates its parameter count sees a DECCRA it has no signature for and
#     silently does not copy.
#
# Both are meant for upstream (ThomasDickey/vttest-snapshots); the fork exists to carry them until
# then, not to diverge. Override with the environment to measure against plain upstream:
#   VTTEST_REPOSITORY=https://github.com/ThomasDickey/vttest-snapshots.git VTTEST_BRANCH=master
REPOSITORY="${VTTEST_REPOSITORY:-https://github.com/contour-terminal/vttest-snapshots.git}"
BRANCH="${VTTEST_BRANCH:-contour-aware}"
TARGET="${1:-$(dirname "$0")/../out/vttest}"

if ! command -v git >/dev/null 2>&1; then
    echo "error: git is required to fetch vttest." >&2
    exit 1
fi

if ! command -v make >/dev/null 2>&1; then
    echo "error: make is required to build vttest." >&2
    exit 1
fi

if [ -d "$TARGET/.git" ]; then
    echo "Updating vttest ($BRANCH) in $TARGET ..."
    # $REPOSITORY is the single source of truth for where vttest comes from, so point origin at it
    # rather than trusting whatever the existing checkout happens to carry. A tree cloned by hand may
    # well have origin on upstream (which has no "contour-aware" branch), and a VTTEST_REPOSITORY
    # override must behave the same here as it does on the clone path below -- silently fetching from
    # the previous remote is the worst outcome, because it looks like it worked.
    git -C "$TARGET" remote set-url origin "$REPOSITORY" 2>/dev/null \
        || git -C "$TARGET" remote add origin "$REPOSITORY"
    git -C "$TARGET" fetch --depth 1 origin "$BRANCH"
    git -C "$TARGET" checkout -B "$BRANCH" FETCH_HEAD
else
    echo "Cloning vttest ($BRANCH) into $TARGET ..."
    git clone --depth 1 --branch "$BRANCH" "$REPOSITORY" "$TARGET"
fi

echo "Building vttest ..."
(cd "$TARGET" && ./configure >/dev/null && make -s >/dev/null)

VTTEST_BIN="$(cd "$TARGET" && pwd)/vttest"
if [ ! -x "$VTTEST_BIN" ]; then
    echo "error: vttest did not build; expected $VTTEST_BIN" >&2
    exit 1
fi

echo
echo "vttest is ready: $VTTEST_BIN"
echo
echo "  Run it against Contour with:"
echo "    vtconformance-run run --suite vttest --program '$VTTEST_BIN'"
echo
echo "  The conformance gate takes whatever vttest is on PATH by default, so a distribution's"
echo "  package works too -- see the fork's fixes above for what it is missing."
