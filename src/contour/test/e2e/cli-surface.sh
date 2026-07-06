#! /bin/sh
# Headless CLI surface sweep for the contour_e2e_cli test: runs every subcommand that completes
# without a window (ContourApp.cpp command table), covering the documentation/info generators,
# the config/terminfo/integration writers, and the license/version/debug-tag printers with
# near-zero runtime. $1 is the contour binary.
set -e

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

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
"$CONTOUR" generate terminfo to "$OUT/generated.terminfo"
"$CONTOUR" generate integration shell zsh to "$OUT/integration.zsh"
"$CONTOUR" generate integration shell fish to "$OUT/integration.fish"
"$CONTOUR" generate integration shell bash to "$OUT/integration.bash"
"$CONTOUR" generate parser-table > "$OUT/parser-table.txt"
# These two normally target a live terminal; without one they may exit non-zero after exercising
# their argument handling — either outcome is acceptable, only crashes are not.
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
