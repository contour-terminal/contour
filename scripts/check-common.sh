#! /bin/bash

set -e

ErrorCount=0

## CI: vcpkgGitCommitId version match

A=$(grep vcpkgGitCommitId .github/workflows/build.yml | awk '{print $2}')
B=$(grep vcpkgGitCommitId .github/workflows/release.yml | awk '{print $2}')

if [[ "${A}" != "${B}" ]]; then
    echo 1>&2 "Error: vcpkgGitCommitId are not matching between CI build and release."
    ErrorCount=$[ErrorCount + 1]
fi

## libunicode version match

LIBUNICODE_SHA_PS=$(awk 'match ($0, /"libunicode-[0-9a-f]+"/) { print(substr($0, RSTART+12, RLENGTH-12-1)); }' scripts/install-deps.ps1)
LIBUNICODE_SHA_SH=$(awk 'match ($0, /libunicode_git_sha="[0-9a-f]+/) { print(substr($0, RSTART+20, RLENGTH-20)); }' scripts/install-deps.sh)

if [[ "${LIBUNICODE_SHA_SH}" != "${LIBUNICODE_SHA_PS}" ]]; then
    echo 1>&2 "Error: libunicode version seems to mismatch between the two install-deps scripts."
    echo 1>&2 "libunicode sha (PowerShell) : ${LIBUNICODE_SHA_SH}"
    echo 1>&2 "libunicode sha (bash)       : ${LIBUNICODE_SHA_PS}"
    ErrorCount=$[ErrorCount + 1]
fi

if [[ $ErrorCount -ne 0 ]]; then
    exit 1
fi

echo "Seems all OK"
