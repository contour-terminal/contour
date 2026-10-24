#! /bin/bash
# Renders the chrome snapshots -- every script, chrome row, defaults edition and appearance in the
# snapshot table -- as PNGs plus a manifest.tsv, under a real compositor (scripts/run-display-tests.sh).
#
# The images are review artefacts, never acceptance: a reviewer looks at them, and no test compares
# them against a baseline.
#
# Two passes, because Qt Quick Controls picks its style once per process: the plain pass renders the
# rows drawn under Fusion, the second (CONTOUR_TEST_CONTROLS_STYLE=ContourTui) the Terminal row.
# The script fails unless the PNG count is non-zero and equals the manifest's line count; the count is
# never written here, because the table owns it.
#
# Usage: scripts/run-chrome-snapshots.sh <path-to-contour_gui_test> <output-dir>

set -euo pipefail

usage() {
    echo "usage: $0 <path-to-contour_gui_test> <output-dir>" >&2
    exit 64
}

[ $# -eq 2 ] || usage
TEST_BINARY="$1"
OUTPUT_DIR="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"
CONTOUR_SNAPSHOT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
export CONTOUR_SNAPSHOT_DIR

# --run keeps run-display-tests.sh from appending its own "[display]" argument, which Catch2 would OR
# with this selection and so run every display case under ContourTui.
"$HERE/run-display-tests.sh" --run "$TEST_BINARY" "[display][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui "$HERE/run-display-tests.sh" --run "$TEST_BINARY" "[display][snapshot]"

PNG_COUNT=$(find "$CONTOUR_SNAPSHOT_DIR" -maxdepth 1 -name '*.png' | wc -l)
MANIFEST="$CONTOUR_SNAPSHOT_DIR/manifest.tsv"
[ -f "$MANIFEST" ] || { echo "error: no manifest written to $MANIFEST" >&2; exit 1; }
MANIFEST_LINES=$(($(wc -l <"$MANIFEST") - 1))

if [ "$PNG_COUNT" -eq 0 ] || [ "$PNG_COUNT" -ne "$MANIFEST_LINES" ]; then
    echo "error: $PNG_COUNT snapshots, but $MANIFEST_LINES manifest entries" >&2
    exit 1
fi
echo "$PNG_COUNT chrome snapshots in $CONTOUR_SNAPSHOT_DIR"
