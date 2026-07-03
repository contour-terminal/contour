#! /bin/bash
# Runs the display-gated GUI tests (the [display] tag, opt-in via CONTOUR_TEST_DISPLAY=1) against a
# REAL compositor. These tests drive a real TerminalDisplay + scene graph and therefore need an
# actual windowing system; the regular offscreen test run skips them.
#
# Compositor selection, in order:
#   1. Caller override: if QT_QPA_PLATFORM is already set, the current environment is used verbatim
#      (e.g. QT_QPA_PLATFORM=xcb DISPLAY=:0 against a live session).
#   2. Headless weston (preferred, CI-grade): a private weston instance with the headless backend —
#      a real, well-behaved Wayland compositor that needs no GPU and no X server, and reliably
#      delivers the frame callbacks that desktop compositors withhold from hidden windows (which is
#      what makes grabWindow()-based tests hang on a live desktop).
#   3. X11 fallback: DISPLAY is set -> QT_QPA_PLATFORM=xcb (XWayland/Xorg deliver frames for
#      unmapped windows).
#
# Usage: scripts/run-display-tests.sh <path-to-contour_gui_test> [extra test binary args...]

set -euo pipefail

TEST_BINARY="${1:?usage: $0 <path-to-contour_gui_test> [args...]}"
shift
[ -x "$TEST_BINARY" ] || { echo "error: $TEST_BINARY is not executable" >&2; exit 64; }

# LeakSanitizer is disabled for the display route ONLY. Under a real compositor, process exit leaves
# ~6.5KB in ~135 allocations entirely inside Qt's QPA/RHI and the Mesa/EGL/wayland driver teardown
# paths — freed by the driver's own atexit hook, which runs AFTER LSan's. Verified to contain ZERO
# first-party frames (no contour/vtbackend/vtrasterizer/vtpty/crispy symbol in any leak block), and
# the driver frames are stripped `<unknown module>` allocations that name/suppression-file matching
# cannot target. Genuine contour leaks are NOT hidden by this: the one this route found — a ~1.9MB
# screenshot QImage retained by the wayland clipboard — was fixed in the test's code (deliver to a
# file, not the clipboard), and ASan proper (use-after-free/overflow) stays fully enabled here. The
# default OFFSCREEN ctest suite keeps full leak checking; only this real-display wrapper relaxes it.
# A caller-provided LSAN_OPTIONS wins.
run_tests()
{
    CONTOUR_TEST_DISPLAY=1 \
    LIBGL_ALWAYS_SOFTWARE=1 \
    ASAN_OPTIONS="hard_rss_limit_mb=4096:${ASAN_OPTIONS:-}" \
    LSAN_OPTIONS="${LSAN_OPTIONS:-detect_leaks=0}" \
        "$TEST_BINARY" "[display]" "$@"
}

# 1. Explicit caller-chosen platform.
if [ -n "${QT_QPA_PLATFORM:-}" ]; then
    echo ">>> using caller-provided QT_QPA_PLATFORM=$QT_QPA_PLATFORM"
    run_tests "$@"
    exit $?
fi

# 2. Private headless weston.
if command -v weston >/dev/null; then
    # weston requires XDG_RUNTIME_DIR (0700). CI runners may not have one.
    if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -w "${XDG_RUNTIME_DIR:-/nonexistent}" ]; then
        XDG_RUNTIME_DIR="$(mktemp -d)"
        export XDG_RUNTIME_DIR
        chmod 700 "$XDG_RUNTIME_DIR"
    fi
    SOCKET="wayland-contour-test-$$"
    echo ">>> starting headless weston on $SOCKET"
    weston --backend=headless --socket="$SOCKET" --width=1280 --height=800 \
        >"$XDG_RUNTIME_DIR/weston-$SOCKET.log" 2>&1 &
    WESTON_PID=$!
    trap 'kill "$WESTON_PID" 2>/dev/null || true' EXIT

    # Wait (bounded) for the compositor socket to appear.
    for _ in $(seq 1 50); do
        [ -S "$XDG_RUNTIME_DIR/$SOCKET" ] && break
        kill -0 "$WESTON_PID" 2>/dev/null || { echo "error: weston died at startup:" >&2;
            cat "$XDG_RUNTIME_DIR/weston-$SOCKET.log" >&2; exit 1; }
        sleep 0.2
    done
    [ -S "$XDG_RUNTIME_DIR/$SOCKET" ] || { echo "error: weston socket never appeared" >&2; exit 1; }

    WAYLAND_DISPLAY="$SOCKET" QT_QPA_PLATFORM=wayland run_tests "$@"
    exit $?
fi

# 3. X11 fallback.
if [ -n "${DISPLAY:-}" ]; then
    echo ">>> weston not found; falling back to QT_QPA_PLATFORM=xcb on DISPLAY=$DISPLAY"
    QT_QPA_PLATFORM=xcb run_tests "$@"
    exit $?
fi

echo "error: no compositor available (install weston for the headless route, or set DISPLAY/QT_QPA_PLATFORM)" >&2
exit 1
