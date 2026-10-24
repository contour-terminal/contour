#! /bin/sh
# Stdio relay sweep for the contour_e2e_stdio_relay test: `contour daemon --stdio`, the remote end of an
# ssh attach, must write nothing to stdout but its preamble -- not even a warning its configuration
# emits while it loads, which is before logging is installed and so goes to the default sink,
# std::cout. A single stray byte there is "remote shell noise" to the client at best and a corrupt
# frame at worst. $1 is the contour binary.
set -e

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

OUT="$(mktemp -d)"
SOCKET="$OUT/sock"
# The developer's own configuration stays out of it, and nothing staged lands in their state directory.
XDG_CONFIG_HOME="$OUT/config"
XDG_STATE_HOME="$OUT/state"
export XDG_CONFIG_HOME XDG_STATE_HOME
DAEMON_PID=""

# The daemon is killed by PID, never by name (see daemon-lifecycle.sh).
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
    for log in "$OUT"/*.err "$OUT/daemon.log"; do
        [ -s "$log" ] && { echo "--- $log ---" >&2; cat "$log" >&2; }
    done
    exit 1
}

# A deprecated key: loading this configuration logs a warning.
WARNING_CONFIG="$OUT/warning.yml"
printf 'images:\n    max_width: 100\n' > "$WARNING_CONFIG"
# Not YAML at all: loading this configuration fails.
BROKEN_CONFIG="$OUT/broken.yml"
printf 'profiles: [\n' > "$BROKEN_CONFIG"

# A daemon of this script's own to relay to, so the relay starts none.
"$CONTOUR" daemon --socket "$SOCKET" --log-file "$OUT/daemon.log" > "$OUT/daemon.out" 2>&1 &
DAEMON_PID=$!
waited=0
while [ ! -e "$SOCKET" ]; do
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "the daemon exited before binding $SOCKET"
    waited=$((waited + 1))
    [ "$waited" -gt 200 ] && fail "the daemon did not bind $SOCKET within 20s"
    sleep 0.1
done

# --- A configuration warning goes to stderr, the preamble alone to stdout -----------------------
#
# An empty stdin ends the relay: it half-closes towards the daemon, which then closes its side.
timeout 20 "$CONTOUR" daemon --stdio --socket "$SOCKET" --config "$WARNING_CONFIG" \
    < /dev/null > "$OUT/warning.out" 2> "$OUT/warning.err" \
    || fail "the relay over a configuration with a warning did not exit 0"
grep -q "max_width" "$OUT/warning.err" || fail "the configuration's warning was not reported on stderr"
printf 'contour-stdio 1 ready\n' > "$OUT/ready.expected"
cmp -s "$OUT/warning.out" "$OUT/ready.expected" \
    || fail "stdout held more than the ready preamble: $(od -c "$OUT/warning.out" | head -5)"

# --- A configuration that does not load is reported to the client too -------------------------
#
# As an error preamble, the one line the attaching client can show, and as the reason on stderr.
if timeout 20 "$CONTOUR" daemon --stdio --socket "$SOCKET" --config "$BROKEN_CONFIG" \
        < /dev/null > "$OUT/broken.out" 2> "$OUT/broken.err"; then
    fail "the relay over a configuration that does not load exited 0"
fi
grep -q "^contour daemon: " "$OUT/broken.err" || fail "the broken configuration's reason was not on stderr"
[ "$(wc -l < "$OUT/broken.out")" -eq 1 ] || fail "stdout held more than one line: $(od -c "$OUT/broken.out" | head -5)"
grep -q "^contour-stdio 1 error: " "$OUT/broken.out" || fail "stdout held no error preamble: $(cat "$OUT/broken.out")"

exit 0
