#! /bin/bash

set -e

A=$(grep vcpkgGitCommitId .github/workflows/build.yml | awk '{print $2}')
B=$(grep vcpkgGitCommitId .github/workflows/release.yml | awk '{print $2}')

if [[ "${A}" != "${B}" ]]; then
    echo 1>&2 "Error: vcpkgGitCommitId are not matching between CI build and release."
    exit 1
fi

echo "Seems all OK"
