#! /bin/sh

set -e

PROJECT_ROOT="$(dirname $0)/.."

cp -vrip "${PROJECT_ROOT}/scripts/git-hooks" \
         "${PROJECT_ROOT}/.git/hooks"
