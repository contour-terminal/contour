#! /bin/bash
#
# Usage: Xvfb-contour-run.sh <dump-dir> <contour-args>

set -x

LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-true}"
CONTOUR_BIN=${CONTOUR_BIN:-contour}
LOG="error,config,pty,gui.session,gui.display,vt.renderer,font.locator" # "all"
LOG="all"
DISPLAY=:99
#CONTOUR_PREFIX=gdb --batch --command=./scripts/test.gdb --args

DUMP_DIR="${1}"
shift

export LIBGL_ALWAYS_SOFTWARE

if [[ ! ~/.terminfo ]] && [[ -d ./build/src/contour/terminfo ]]
then
    ln -sf ./build/src/contour/terminfo ~/.terminfo
fi

Xvfb $DISPLAY -screen 0 1280x1024x24 &
XVFB_PID=$!
trap "kill $XVFB_PID" EXIT

sleep 3

ldd `which $CONTOUR_BIN`

export CONTOUR_SYNC_PTY_OUTPUT=1

$CONTOUR_PREFIX \
    $CONTOUR_BIN terminal \
        debug "$LOG" \
        display ${DISPLAY} \
        early-exit-threshold 0 \
        ${@}

# Capture exit code immediately before any other command overwrites $?
EXIT_CODE=$?

# ~/opt/notcurses/bin/notcurses-demo -p ~/opt/notcurses/share/notcurses

if [[ -n "$GITHUB_OUTPUT" ]]; then
    echo "exitCode=$EXIT_CODE" >> "$GITHUB_OUTPUT"
fi

# Exit with the actual exit code so the CI step itself fails on error
exit $EXIT_CODE
