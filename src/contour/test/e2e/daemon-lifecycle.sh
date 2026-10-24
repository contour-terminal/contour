#! /bin/sh
# Daemon lifecycle sweep for the contour_e2e_daemon test: boots a real `contour daemon`, proves it
# binds and releases its control socket, refuses a second daemon on the same socket, and refuses
# the two configurations that must never start. $1 is the contour binary.
#
# The unit suites (vthost_test, and core-cpp's core::net suite) drive the daemon's internals through injected sockets and
# a test event loop. Nothing there starts the actual binary, so the wiring between the CLI verb and
# vthost::runDaemon -- argument validation, socket path derivation, signal handling, unlink-on-close
# -- had no coverage at all. That is what this sweep is for.
set -e

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

OUT="$(mktemp -d)"
SOCKET="$OUT/sock"
# Anything this script stages (a daemon's image copy, its lease) lands in the temp dir, never in the
# real state directory.
XDG_STATE_HOME="$OUT/state"
export XDG_STATE_HOME
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

# An unknown --lifecycle is rejected for the same reason, naming the option.
if timeout 10 "$CONTOUR" daemon --socket "$SOCKET" --lifecycle=nosuch \
        > "$OUT/bad-lifecycle.txt" 2>&1; then
    fail "an unknown --lifecycle unexpectedly started a daemon"
fi
grep -q "lifecycle" "$OUT/bad-lifecycle.txt" \
    || fail "the --lifecycle rejection does not name the offending option"

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

# An access file is checked before anything binds: a bad grant names its line, and the option alone
# narrows nothing.
printf 'read,fly t0ken\n' > "$OUT/bad-access"
if timeout 10 "$CONTOUR" daemon --socket "$SOCKET" --listen-tcp 127.0.0.1:9099 \
        --access-file "$OUT/bad-access" > "$OUT/bad-access.txt" 2>&1; then
    fail "a malformed --access-file unexpectedly started a daemon"
fi
grep -q "line 1" "$OUT/bad-access.txt" \
    || fail "the --access-file rejection does not name the offending line"
printf 'read,screen t0ken\n' > "$OUT/access"
if timeout 10 "$CONTOUR" daemon --socket "$SOCKET" --access-file "$OUT/access" \
        > "$OUT/access-alone.txt" 2>&1; then
    fail "--access-file without --listen-tcp unexpectedly started a daemon"
fi
grep -q "listen-tcp" "$OUT/access-alone.txt" \
    || fail "the --access-file rejection does not name --listen-tcp"

[ -e "$SOCKET" ] && fail "a refused daemon left a socket behind at $SOCKET"

# --- A daemon binds its socket ---------------------------------------------------------------
#
# Foreground, backgrounded by this shell, so $! is the daemon's own pid: exact, and safe to signal.
# (`--background` re-execs a detached copy and reports success on stdout, but hands back no pid,
# which would leave this script guessing which process to stop.)
"$CONTOUR" daemon --socket "$SOCKET" --log-file "$OUT/daemon.log" > "$OUT/daemon.out" 2>&1 &
DAEMON_PID=$!

waited=0
while [ ! -e "$SOCKET" ]; do
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "the daemon exited before binding $SOCKET"
    waited=$((waited + 1))
    [ "$waited" -gt 200 ] && fail "the daemon did not bind $SOCKET within 20s"
    sleep 0.1
done

# --- A Linux install runs in place --------------------------------------------------------------
#
# A build tree or a packaged binary is no AppImage: an upgrade replaces it by rename, which a running
# ELF survives, so the daemon is not re-executed from a per-version copy and nothing is staged. (The
# staging and re-exec path is covered by vthost_test's [relocate] cases, and for an AppImage by the
# manual check in docs/internals/vthost.md.)
if [ -e "/proc/$DAEMON_PID/exe" ]; then
    RUNNING="$(readlink "/proc/$DAEMON_PID/exe")"
    STARTED="$(readlink -f "$CONTOUR")"
    [ "$RUNNING" = "$STARTED" ] || fail "the daemon runs from $RUNNING, not from $STARTED"
fi
[ -e "$XDG_STATE_HOME/contour/bin" ] && fail "a Linux install staged a daemon image into $XDG_STATE_HOME/contour/bin"

# --- A second daemon on the same socket is refused --------------------------------------------
#
# net::listenUnix probes the path by connecting to it first, so a live incumbent answers and the
# newcomer gets AddressInUse rather than stealing the path out from under it.
if timeout 20 "$CONTOUR" daemon --socket "$SOCKET" > "$OUT/second.txt" 2>&1; then
    fail "a second daemon on a live socket unexpectedly succeeded"
fi
kill -0 "$DAEMON_PID" 2>/dev/null || fail "the incumbent daemon died when a second one probed it"

# --- `contour daemon status | logs | stop` ---------------------------------------------------------
#
# The verbs that talk to the daemon rather than become one. The daemon above logs to a file
# (--log-file), which is what `logs` reads back through the daemon's own status.
"$CONTOUR" daemon status --socket "$SOCKET" > "$OUT/status.txt" 2>&1 \
    || fail "daemon status against a running daemon did not exit 0"
grep -q "sessions:" "$OUT/status.txt" || fail "daemon status does not report the sessions"
grep -q "pid $DAEMON_PID" "$OUT/status.txt" || fail "daemon status does not name the daemon's pid $DAEMON_PID"

"$CONTOUR" daemon logs --socket "$SOCKET" > "$OUT/logs.txt" 2>&1 \
    || fail "daemon logs against a running daemon did not exit 0"
grep -q "serving on" "$OUT/logs.txt" || fail "daemon logs does not print the daemon's log"

# --yes: the idle test daemon hosts no sessions, but the flag must be accepted either way.
"$CONTOUR" daemon stop --socket "$SOCKET" --yes > "$OUT/stop.txt" 2>&1 \
    || fail "daemon stop --yes did not exit 0"
waited=0
while kill -0 "$DAEMON_PID" 2>/dev/null; do
    waited=$((waited + 1))
    [ "$waited" -gt 50 ] && fail "the daemon was still running 5s after daemon stop"
    sleep 0.1
done
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""

# No daemon is running now: status says so with the unreachable status, 4.
status=0
"$CONTOUR" daemon status --socket "$SOCKET" > "$OUT/status-absent.txt" 2>&1 || status=$?
[ "$status" -eq 4 ] || fail "daemon status with no daemon exited $status, not 4"
grep -q "no daemon is running" "$OUT/status-absent.txt" \
    || fail "daemon status with no daemon does not say so"

# --- Termination releases the socket ----------------------------------------------------------
#
# SIGTERM, not SIGKILL: the point is that the pump loop is exitable and the listener unlinks its
# path on the way out. A SIGKILL would skip both and pass for the wrong reason.
"$CONTOUR" daemon --socket "$SOCKET" --log-file "$OUT/daemon.log" > "$OUT/daemon.out" 2>&1 &
DAEMON_PID=$!
waited=0
while [ ! -e "$SOCKET" ]; do
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "the daemon exited before binding $SOCKET"
    waited=$((waited + 1))
    [ "$waited" -gt 200 ] && fail "the daemon did not bind $SOCKET within 20s"
    sleep 0.1
done
kill "$DAEMON_PID"
wait "$DAEMON_PID" 2>/dev/null || true

waited=0
while [ -e "$SOCKET" ]; do
    waited=$((waited + 1))
    [ "$waited" -gt 100 ] && fail "the daemon did not remove $SOCKET on termination"
    sleep 0.1
done
DAEMON_PID=""

# --- An on-demand daemon ends once it is idle --------------------------------------------------
#
# --lifecycle=exit-when-idle starts with no session and no client, so it is idle from the first
# instant and must be gone one grace period (10 s) later without any signal.
"$CONTOUR" daemon --socket "$SOCKET" --lifecycle=exit-when-idle > "$OUT/daemon.log" 2>&1 &
DAEMON_PID=$!

waited=0
while kill -0 "$DAEMON_PID" 2>/dev/null; do
    waited=$((waited + 1))
    [ "$waited" -gt 200 ] && fail "an idle exit-when-idle daemon was still running after 20s"
    sleep 0.1
done
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
if [ -e "$SOCKET" ]; then
    fail "the idle daemon left $SOCKET behind"
fi

# --- A backgrounded daemon into a socket directory that does not exist yet -----------------------
#
# Without --log-file it logs beside its socket. Were that log file's directory created first, by the
# log sink, it would get the umask's 0755 and the listener would refuse to bind in it -- as after a
# reboot, when $XDG_RUNTIME_DIR/contour is gone. The daemon makes the directory owner-only before
# anything else touches it. Started exit-when-idle, it ends on its own: `--background` hands back no
# pid to stop it by.
FRESH="$OUT/fresh"
if ! timeout 30 "$CONTOUR" daemon --background --socket "$FRESH/sock" --lifecycle=exit-when-idle \
        > "$OUT/background.txt" 2>&1; then
    cat "$OUT/background.txt" >&2
    [ -s "$FRESH/sock.log" ] && cat "$FRESH/sock.log" >&2
    fail "a backgrounded daemon did not start in a fresh socket directory"
fi
grep -q "serving on" "$OUT/background.txt" || fail "the backgrounded daemon did not report serving"
[ "$(stat -c %a "$FRESH")" = "700" ] || fail "the fresh socket directory is $(stat -c %a "$FRESH"), not 700"
[ -s "$FRESH/sock.log" ] || fail "the backgrounded daemon did not log beside its socket"

waited=0
while [ -e "$FRESH/sock" ]; do
    waited=$((waited + 1))
    [ "$waited" -gt 200 ] && fail "the backgrounded exit-when-idle daemon was still serving after 20s"
    sleep 0.1
done
