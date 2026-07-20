#! /bin/bash
#
# Validates every .clang-tidy in the tree against the installed clang-tidy.
#
# `clang-tidy --verify-config` only inspects the config it discovers for the
# current working directory, so each config directory is checked in turn.
# Unknown option keys are reported as warnings (not a non-zero exit), which is
# how a misspelled key such as `VariableCasePrefix` can silently no-op for
# years -- so any output other than the all-clear is treated as a failure.

set -uo pipefail

CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
failed=0

while IFS= read -r config; do
    dir="$(dirname "$config")"
    echo "==> ${config}"
    output="$(cd "$dir" && "$CLANG_TIDY" --verify-config 2>&1)"
    status=$?
    echo "$output"
    if [[ $status -ne 0 || "$output" != "No config errors detected." ]]; then
        failed=1
    fi
done < <(find . -name .clang-tidy -not -path './_deps/*' -not -path './out/*' | sort)

if [[ $failed -ne 0 ]]; then
    echo 1>&2 "Error: clang-tidy configuration is not clean."
    exit 1
fi

echo "All good. ;-)"
