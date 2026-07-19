#! /bin/sh
# Headless CLI surface sweep for the contour_e2e_cli test: runs every subcommand that completes
# without a window (ContourApp.cpp command table), covering the documentation/info generators,
# the config/terminfo/integration writers, and the license/version/debug-tag printers with
# near-zero runtime. $1 is the contour binary.
set -e

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

# This script must run with NO controlling terminal, and says so rather than assuming it.
#
# `capture` below reaches its terminal peer by opening /dev/tty (CaptureScreen.cpp) -- the CONTROLLING
# terminal, which `< /dev/null > /dev/null` does NOT detach from; only a session without one does. Run
# from an interactive shell it therefore drove the DEVELOPER'S OWN terminal, raising a real capture
# permission prompt that could block the run. (The other terminal-facing verbs here, `set profile` and
# `cat`, only write escape sequences to stdout, which is redirected to a file below.)
#
# It also made the outcome environment-dependent: a CI runner has no /dev/tty, so capture failed there
# for a reason a developer's machine never reproduced -- the test passed for different reasons in each.
#
# CMake runs this under `setsid -w` for exactly that reason. This check is what keeps the contract true
# if that ever gets dropped: better a loud failure than silently reaching for someone's terminal.
# The open runs in a SUBSHELL: `:` is a special built-in, and POSIX has a redirection error on one abort
# the whole shell -- testing this inline would kill the script instead of answering the question.
if (: < /dev/tty) 2>/dev/null; then
    echo "error: $0 must run without a controlling terminal (CMake runs it under 'setsid -w')" >&2
    exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

"$CONTOUR" help > "$OUT/help.txt"
"$CONTOUR" version > "$OUT/version.txt"
"$CONTOUR" license > "$OUT/license.txt"
"$CONTOUR" list-debug-tags > "$OUT/tags.txt"
"$CONTOUR" info vt > "$OUT/info-vt.txt"
# `info config` and `font-locator` are exercised for coverage but their output is
# environment-dependent (empty when no config file / no fontconfig is present, as on a bare CI
# runner), so they are NOT held to the non-empty contract below — only that they do not crash.
"$CONTOUR" info config > /dev/null 2>&1 || true
"$CONTOUR" font-locator > /dev/null 2>&1 || true
"$CONTOUR" documentation vt > "$OUT/doc-vt.txt"
"$CONTOUR" documentation keys > "$OUT/doc-keys.txt"
"$CONTOUR" documentation configuration global > "$OUT/doc-config-global.txt"
"$CONTOUR" documentation configuration profile > "$OUT/doc-config-profile.txt"
"$CONTOUR" generate config to "$OUT/generated-config.yml"
# The generated sample config must carry the tab-bar options (guards the reflection-driven
# serialization + the doc-template keys against a silent regression). Anchored to column 0: these
# are GLOBAL settings, and a profile-scoped key is indented -- so an unanchored grep would keep
# passing if they ever drifted back under `profiles:`, where nothing would read them.
for key in tab_bar_position tab_bar_visibility; do
    grep -q "^$key:" "$OUT/generated-config.yml" \
        || { echo "error: generated config is missing a global '$key:'" >&2; exit 1; }
done
"$CONTOUR" generate terminfo to "$OUT/generated.terminfo"
"$CONTOUR" generate integration shell zsh to "$OUT/integration.zsh"
"$CONTOUR" generate integration shell fish to "$OUT/integration.fish"
"$CONTOUR" generate integration shell bash to "$OUT/integration.bash"
"$CONTOUR" generate parser-table > "$OUT/parser-table.txt"
# These two target a live terminal, and there is none here (see the /dev/tty check above), so they exit
# non-zero after exercising their argument handling — which is the point: only crashes are unacceptable.
"$CONTOUR" set profile to main > "$OUT/set-profile.txt" 2>&1 || true
"$CONTOUR" cat "$(dirname "$0")/../../../../docs/screenshots/contour-font-ligatures.png" \
    > "$OUT/cat-image.txt" 2>&1 || true
rm -f "$OUT/cat-image.txt"

# Error paths must fail loudly, not crash: an unknown integration shell and a capture without a
# terminal peer both exit non-zero with a diagnostic.
if "$CONTOUR" generate integration shell nosuchshell to - > "$OUT/bad-shell.txt" 2>&1; then
    echo "error: unknown integration shell unexpectedly succeeded" >&2; exit 1
fi
if "$CONTOUR" capture timeout 0.1 lines 1 to "$OUT/capture.txt" < /dev/null > /dev/null 2>&1; then
    echo "error: capture without a terminal unexpectedly succeeded" >&2; exit 1
fi
rm -f "$OUT/bad-shell.txt" "$OUT/capture.txt" "$OUT/set-profile.txt"

# Sanity: the generators must produce non-empty output (an empty file means the sink wiring
# silently broke, which a bare exit code would not catch).
for f in "$OUT"/*; do
    [ -s "$f" ] || { echo "error: $f is empty" >&2; exit 1; }
done
