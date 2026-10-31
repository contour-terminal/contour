#!/usr/bin/env bash
# Runs clang-tidy on the lines this branch changed relative to its merge-base with a base ref.
#
#   scripts/clang-tidy-diff.sh [--base REF] [--build-dir DIR]
#
# Defaults: --base origin/master, --build-dir out/clang-asan. CLANG_TIDY names the binary
# (default clang-tidy); JOBS caps the parallel runs (default: nproc).
#
# scripts/tidy-diff-plan.py decides which translation units cover the changed lines (a changed
# header is linted through one TU that includes it). A changed header no TU includes fails the run
# rather than being skipped. Lines inside an inactive #if region of a header are not analysed (as in
# the full-tree run). The build dir needs compile_commands.json and the generated headers
# (moc, autogen) the linted TUs include; it does not need to be fully built.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
base="origin/master"
build_dir="out/clang-asan"
tidy="${CLANG_TIDY:-clang-tidy}"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --base) base="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "clang-tidy-diff: unknown argument: $1" >&2; exit 64 ;;
    esac
done

cd "$root"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

merge_base="$(git merge-base "$base" HEAD)"
git diff -U0 --no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ "$merge_base" -- >"$work/diff"

plan_status=0
python3 scripts/tidy-diff-plan.py --build-dir "$build_dir" --diff "$work/diff" --tsv >"$work/plan.tsv" || plan_status=$?
if [[ $plan_status -ne 0 ]]; then
    echo "clang-tidy-diff: planning failed (exit $plan_status)" >&2
    exit "$plan_status"
fi
if [[ ! -s "$work/plan.tsv" ]]; then
    echo "clang-tidy-diff: no changed first-party lines to lint."
    exit 0
fi

index=0
while IFS=$'\t' read -r tu filter; do
    index=$((index + 1))
    (
        if ! "$tidy" -p "$build_dir" --quiet -line-filter="$filter" "$tu" >"$work/out.$index" 2>&1 \
            || grep -qE ': (warning|error):' "$work/out.$index"; then
            : >"$work/failed.$index"
        fi
    ) &
    while [[ $(jobs -rp | wc -l) -ge $jobs ]]; do wait -n || true; done
done <"$work/plan.tsv"
wait

status=0
for out in "$work"/out.*; do
    suffix="${out##*.}"
    if [[ -e "$work/failed.$suffix" ]]; then
        cat "$out"
        status=1
    fi
done
if [[ $status -eq 0 ]]; then
    echo "clang-tidy-diff: $(wc -l <"$work/plan.tsv") translation unit(s) clean."
else
    echo "clang-tidy-diff: findings on changed lines (see above)." >&2
fi
exit "$status"
