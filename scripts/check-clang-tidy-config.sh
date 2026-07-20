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
checked=0

# The set of configs comes from git, rooted at the repository rather than at $PWD, for two reasons.
# Invoked from a subdirectory, a `find .` matches nothing at all and the loop below falls straight
# through to the all-clear without having verified anything -- a green run that checked nothing is
# worse than no check. And only tracked files are ours to keep clean: vendored trees (vcpkg_installed/,
# _deps/, out/) ship their own .clang-tidy files, so whether a build tree happens to be present must
# not decide whether this check passes.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel 2>/dev/null)"

if [[ -z "$repo_root" ]]; then
    echo 1>&2 "Error: ${script_dir} is not inside a git repository."
    exit 1
fi

while IFS= read -r config; do
    dir="$(dirname "${repo_root}/${config}")"
    echo "==> ${config}"
    output="$(cd "$dir" && "$CLANG_TIDY" --verify-config 2>&1)"
    status=$?
    echo "$output"
    checked=$((checked + 1))
    if [[ $status -ne 0 || "$output" != "No config errors detected." ]]; then
        failed=1
    fi
done < <(git -C "$repo_root" ls-files -- '*.clang-tidy' | sort)

if [[ $checked -eq 0 ]]; then
    echo 1>&2 "Error: no tracked .clang-tidy files found in ${repo_root}."
    exit 1
fi

if [[ $failed -ne 0 ]]; then
    echo 1>&2 "Error: clang-tidy configuration is not clean."
    exit 1
fi

echo "All good. ;-)"
