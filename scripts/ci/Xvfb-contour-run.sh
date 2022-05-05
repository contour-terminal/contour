#! /bin/bash

set -x

export LIBGL_ALWAYS_SOFTWARE='true'

if [[ ! ~/.terminfo ]]
then
    ln -sf ./build/src/contour/terminfo ~/.terminfo
fi

DISPLAY=:99

Xvfb $DISPLAY -screen 0 1280x1024x24 &
XVFB_PID=$!

sleep 3

DUMP_DIR="${1}"
shift

ldd `which contour`

LOG="config,pty,gui.session,gui.display,vt.renderer,font.locator"
#LOG="all"

CONTOUR_PREFIX=gdb --batch --command=./scripts/test.gdb --args

$CONTOUR_PREFIX \
    contour terminal \
        debug "$LOG" \
        display ${DISPLAY} \
        early-exit-threshold 0 \
        dump-state-at-exit "${DUMP_DIR}" \
        ${@}

# ~/opt/notcurses/bin/notcurses-demo -p ~/opt/notcurses/share/notcurses

echo "::set-output name=exitCode::$?"

kill "${XVFB_PID}"

