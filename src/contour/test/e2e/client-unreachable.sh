#! /bin/sh
# Unreachable-daemon sweep for the contour_e2e_client_unreachable test: `contour client` aimed at a
# TCP port nothing listens on, directly or as an ssh host (`--host`), must fail the attach with its
# reason and exit 1 -- not crash on the way out. $1 is the contour binary.
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

# `--host` names the daemon, the way to it and what is mirrored, so an option naming another of these is
# refused, not ignored. $1 is the option, $2 its value.
expectRefusedBesideHost() {
    status=0
    timeout 30 "$CONTOUR" client --host ssh://127.0.0.1:1 "--$1" "$2" >"$OUT/host-both.log" 2>&1 \
        || status=$?
    if [ "$status" -ne 1 ] || ! grep -q "^contour client: pass either --host or --$1, not both" "$OUT/host-both.log"; then
        echo "error: --host beside --$1 was not refused (exit $status)" >&2
        cat "$OUT/host-both.log" >&2
        exit 1
    fi
}
expectRefusedBesideHost connect-tcp 127.0.0.1:1
expectRefusedBesideHost window 3

# An ssh host nothing answers for fails the same way, its reason naming the host once, from ssh's own words.
if command -v ssh >/dev/null 2>&1; then
    status=0
    timeout 60 "$CONTOUR" client --host ssh://127.0.0.1:1 >"$OUT/host.log" 2>&1 || status=$?
    if [ "$status" -ne 1 ]; then
        echo "error: --host to an unreachable ssh host: expected exit status 1, got $status" >&2
        cat "$OUT/host.log" >&2
        exit 1
    fi
    if ! grep -q "^contour client: ssh could not reach ssh://127.0.0.1:1: " "$OUT/host.log" \
            || grep -q "^contour client: .*(ssh://127.0.0.1:1)$" "$OUT/host.log"; then
        echo "error: the client did not report the unreachable ssh host once" >&2
        cat "$OUT/host.log" >&2
        exit 1
    fi
fi
exit 0
