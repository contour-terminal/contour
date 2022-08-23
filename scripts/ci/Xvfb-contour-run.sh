#! /bin/bash
#
# Usage: Xvfb-contour-run.sh <dump-dir> <contour-args>

set -x

LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-true}"
CONTOUR_BIN=${CONTOUR_BIN:-contour}
LOG="error,config,pty,gui.session,gui.display,vt.renderer,font.locator" # "all"
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

$CONTOUR_PREFIX \
    $CONTOUR_BIN terminal \
        debug "$LOG" \
        display ${DISPLAY} \
        early-exit-threshold 0 \
        dump-state-at-exit "${DUMP_DIR}" \
        ${@}

# ~/opt/notcurses/bin/notcurses-demo -p ~/opt/notcurses/share/notcurses

if [[ "$GITHUB_OUTPUT" != "" ]]; then
    echo "exitCode=$?" >> "$GITHUB_OUTPUT"
fi
