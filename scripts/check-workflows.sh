#! /bin/bash
# Validates the GitHub Actions workflow files with actionlint — the same schema check GitHub runs
# when it compiles a workflow. Catches locally the class of error that silently breaks CI dispatch
# (e.g. an invalid `${{ env.X }}` context in a job-level `env:` block makes GitHub create a
# check-suite with zero jobs and fail at 0s, which is invisible without this check).
#
# actionlint is fetched on demand into the build tree if not already on PATH, so this needs no
# system install. If it cannot be obtained (offline), the check SKIPS rather than fails, matching
# the other optional check-*.sh scripts.
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SOURCE_DIR"

# Only workflow files that actually exist; nothing to do otherwise.
if [ ! -d .github/workflows ]; then
    echo "No .github/workflows directory; nothing to lint."
    exit 0
fi

# Resolve actionlint: PATH first, then a cached copy under the build tree.
ACTIONLINT=""
if command -v actionlint >/dev/null 2>&1; then
    ACTIONLINT="$(command -v actionlint)"
else
    CACHE_DIR="${ACTIONLINT_CACHE_DIR:-$SOURCE_DIR/out/.tools}"
    if [ -x "$CACHE_DIR/actionlint" ]; then
        ACTIONLINT="$CACHE_DIR/actionlint"
    else
        mkdir -p "$CACHE_DIR"
        # Official installer script; pins the latest release binary for the host platform.
        if curl -fsSL https://raw.githubusercontent.com/rhysd/actionlint/main/scripts/download-actionlint.bash \
             -o "$CACHE_DIR/download-actionlint.bash" 2>/dev/null \
           && ( cd "$CACHE_DIR" && bash download-actionlint.bash >/dev/null 2>&1 ) \
           && [ -x "$CACHE_DIR/actionlint" ]; then
            ACTIONLINT="$CACHE_DIR/actionlint"
        fi
    fi
fi

if [ -z "$ACTIONLINT" ]; then
    echo "actionlint not available and could not be downloaded; skipping workflow lint." >&2
    exit 0
fi

# Only fail on ERRORS that break workflow compilation, not on style warnings about pinned action
# versions or shellcheck nits (which the repo carries pre-existing and which do not stop dispatch).
# actionlint groups those under specific rule ids; -ignore filters them by message pattern.
"$ACTIONLINT" \
    -ignore 'the runner of ".*" action is too old' \
    -ignore 'property ".*" is not defined in object type' \
    -ignore 'shellcheck reported issue' \
    -ignore 'constant expression .* in condition' \
    -ignore 'label "ubuntu-26\.04.*" is unknown' \
    .github/workflows/*.yml
echo "actionlint: workflows OK."
