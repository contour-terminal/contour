#! /bin/sh
# Daemon lifecycle sweep for the contour_e2e_daemon test: boots a real `contour daemon`, proves it
# binds and releases its control socket, refuses a second daemon on the same socket, and refuses
# the two configurations that must never start. $1 is the contour binary.
#
# The unit suites (vthost_test, net_test) drive the daemon's internals through injected sockets and
# a test event loop. Nothing there starts the actual binary, so the wiring between the CLI verb and
# vthost::runDaemon -- argument validation, socket path derivation, signal handling, unlink-on-close
# -- had no coverage at all. That is what this sweep is for.
set -e

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

OUT="$(mktemp -d)"
SOCKET="$OUT/sock"
DAEMON_PID=""

# The daemon is killed by PID, never by name: `pkill -f contour` on a developer's machine
# SIGKILLs their daily driver. $DAEMON_PID is the shell's own child, so it cannot name anything
# else. Runs on every exit path, including the `set -e` ones.
cleanup() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$OUT"
}
trap cleanup EXIT

fail() {
    echo "error: $1" >&2
    [ -s "$OUT/daemon.log" ] && { echo "--- daemon log ---" >&2; cat "$OUT/daemon.log" >&2; }
    exit 1
}

# --- Configurations that must be refused before anything binds -------------------------------
#
# ContourApp::daemonAction validates everything BEFORE it forks or binds, so a rejected start must
# cost nothing: non-zero exit, and no socket left on disk. `timeout` bounds the damage if that ever
# regresses into a daemon that starts and serves anyway -- without it the run would sit until
# ctest's own TIMEOUT, holding the RUN_SERIAL slot.

# An unknown --size-policy is rejected rather than silently defaulted: serving `latest` to someone
# who asked for `smallest` looks exactly like the feature not working.
if timeout 10 "$CONTOUR" daemon --socket "$SOCKET" --size-policy nosuchpolicy \
        > "$OUT/bad-policy.txt" 2>&1; then
    fail "an unknown --size-policy unexpectedly started a daemon"
fi
grep -q "size-policy" "$OUT/bad-policy.txt" \
    || fail "the --size-policy rejection does not name the offending option"

# A malformed --listen-tcp is caught before the token is even looked at.
if timeout 10 "$CONTOUR" daemon --socket "$SOCKET" --listen-tcp 127.0.0.1:0 \
        > "$OUT/bad-hostport.txt" 2>&1; then
    fail "a malformed --listen-tcp unexpectedly started a daemon"
fi
grep -q "listen-tcp" "$OUT/bad-hostport.txt" \
    || fail "the --listen-tcp rejection does not name the offending option"

# The security-critical one. A TCP listener has no filesystem permissions to fall back on, so
# starting one without a preshared token would hand a full shell to anyone who can reach the port.
# This must refuse to start -- a warning would be dismissed once and then run that way forever.
# The port is well-formed here precisely so the run reaches the token gate rather than stopping at
# the parse above; nothing binds it, because the refusal happens before any listener is created.
if timeout 10 "$CONTOUR" daemon --socket "$SOCKET" --listen-tcp 127.0.0.1:9099 \
        > "$OUT/no-token.txt" 2>&1; then
    fail "--listen-tcp without a token unexpectedly started a daemon"
fi
grep -q "token" "$OUT/no-token.txt" \
    || fail "the --listen-tcp rejection does not mention the missing token"

[ -e "$SOCKET" ] && fail "a refused daemon left a socket behind at $SOCKET"

# --- A daemon binds its socket ---------------------------------------------------------------
#
# Foreground, backgrounded by this shell, so $! is the daemon's own pid: exact, and safe to signal.
# (`--background` re-execs a detached copy and reports success on stdout, but hands back no pid,
# which would leave this script guessing which process to stop.)
"$CONTOUR" daemon --socket "$SOCKET" > "$OUT/daemon.log" 2>&1 &
DAEMON_PID=$!

waited=0
while [ ! -e "$SOCKET" ]; do
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "the daemon exited before binding $SOCKET"
    waited=$((waited + 1))
    [ "$waited" -gt 200 ] && fail "the daemon did not bind $SOCKET within 20s"
    sleep 0.1
done

# --- A second daemon on the same socket is refused --------------------------------------------
#
# net::listenUnix probes the path by connecting to it first, so a live incumbent answers and the
# newcomer gets AddressInUse rather than stealing the path out from under it.
if timeout 20 "$CONTOUR" daemon --socket "$SOCKET" > "$OUT/second.txt" 2>&1; then
    fail "a second daemon on a live socket unexpectedly succeeded"
fi
kill -0 "$DAEMON_PID" 2>/dev/null || fail "the incumbent daemon died when a second one probed it"

# --- Termination releases the socket ----------------------------------------------------------
#
# SIGTERM, not SIGKILL: the point is that the pump loop is exitable and the listener unlinks its
# path on the way out. A SIGKILL would skip both and pass for the wrong reason.
kill "$DAEMON_PID"
wait "$DAEMON_PID" 2>/dev/null || true

waited=0
while [ -e "$SOCKET" ]; do
    waited=$((waited + 1))
    [ "$waited" -gt 100 ] && fail "the daemon did not remove $SOCKET on termination"
    sleep 0.1
done
DAEMON_PID=""
