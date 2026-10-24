#! /bin/sh
# Unreachable-daemon sweep for the contour_e2e_client_unreachable test: `contour client` aimed at a
# TCP port nothing listens on must fail the attach with its reason and exit 1 -- not crash on the way
# out. $1 is the contour binary.
#
# The attach is awaited after the QML engine exists, so this is the one return from the GUI verb that
# leaves before the event loop runs; a Qt object outliving QApplication there crashes or warns at exit.
# Port 1 (tcpmux) is reserved and has no listener on any machine this runs on, so the connect is
# refused at once.

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
# The developer's own configuration stays out of it, and nothing staged lands in their state directory.
XDG_CONFIG_HOME="$OUT/config"
XDG_STATE_HOME="$OUT/state"
export XDG_CONFIG_HOME XDG_STATE_HOME

# `timeout` bounds a regression into a client that waits forever, which would otherwise hold the
# RUN_SERIAL slot until ctest's own TIMEOUT.
timeout 30 "$CONTOUR" client --connect-tcp 127.0.0.1:1 >"$OUT/out.log" 2>&1
status=$?

if [ "$status" -ne 1 ]; then
    echo "error: expected exit status 1, got $status" >&2
    cat "$OUT/out.log" >&2
    exit 1
fi
# The reason is printed, naming the transport it failed on, and nothing Qt reports at teardown follows it.
if ! grep -q "^contour client: .*(tls://127.0.0.1:1)" "$OUT/out.log"; then
    echo "error: the client printed no reason for the failed attach" >&2
    cat "$OUT/out.log" >&2
    exit 1
fi
if grep -qiE "QApplication|destroyed while|still in use|QObject::|ASSERT" "$OUT/out.log"; then
    echo "error: the client warned at teardown:" >&2
    cat "$OUT/out.log" >&2
    exit 1
fi
exit 0
