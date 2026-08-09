#! /bin/bash

set -e

# A check that silently checks nothing is worse than no check, and relative paths are how that
# happens -- so resolve the repo root rather than trusting $PWD.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel 2>/dev/null)"

if [[ -z "$repo_root" ]]; then
    echo 1>&2 "Error: ${script_dir} is not inside a git repository."
    exit 1
fi

ErrorCount=0

## libunicode version

# Neither install-deps script pins libunicode's version: both read it out of the single
# LIBUNICODE_MINIMAL_VERSION line in cmake/ContourThirdParties.cmake, so there is no pair of literals
# left to compare -- and restating their patterns here would only compare this file against itself.
# Each script now rejects an unreadable version where it reads it, which covers every platform rather
# than just CI. What is left worth doing here is failing fast, in the cheap lint job, on the two ways
# that arrangement breaks: the line stops being parseable, or a script stops consulting it.

ThirdPartiesCMakeFile="${repo_root}/cmake/ContourThirdParties.cmake"

LibUnicodeVersion=$(sed -n 's/^set(LIBUNICODE_MINIMAL_VERSION "\([0-9.]*\)".*/\1/p' "${ThirdPartiesCMakeFile}")

if [[ -z "${LibUnicodeVersion}" ]]; then
    echo 1>&2 "Error: cannot read LIBUNICODE_MINIMAL_VERSION from cmake/ContourThirdParties.cmake."
    echo 1>&2 "Expected a line of the form: set(LIBUNICODE_MINIMAL_VERSION \"<version>\")"
    ErrorCount=$((ErrorCount + 1))
fi

for script in scripts/install-deps.sh scripts/install-deps.ps1; do
    if ! grep -q 'LIBUNICODE_MINIMAL_VERSION' "${repo_root}/${script}"; then
        echo 1>&2 "Error: ${script} no longer derives libunicode's version from"
        echo 1>&2 "cmake/ContourThirdParties.cmake, so the two can drift apart again."
        ErrorCount=$((ErrorCount + 1))
    fi
done

if [[ $ErrorCount -ne 0 ]]; then
    exit 1
fi

echo "Seems all OK"
