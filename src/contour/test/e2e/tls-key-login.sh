#! /bin/sh
# Key login over TLS, for the contour_e2e_tls_key_login test: a real `contour daemon --listen-tcp
# --auth keys` and a real control client that signs in with a key held by an ssh-agent, pinning the
# daemon on first contact. $1 is the contour binary.
#
# What it proves that the unit suites cannot: the wiring of the CLI options (--auth, --authorized-keys,
# --daemon-fingerprint), the daemon's TLS identity reaching the client as a fingerprint, the pin file a
# first contact writes, and the refusals of a client that was not told whom to trust or holds no key.
#
# Skips (exit 77) where ssh-agent, ssh-keygen or ssh-add are missing.
set -e

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }
for tool in ssh-agent ssh-keygen ssh-add; do
    command -v "$tool" > /dev/null 2>&1 || { echo "skipped: $tool is not installed" >&2; exit 77; }
done

OUT="$(mktemp -d)"
# The developer's own state, configuration and agent stay out of it.
XDG_STATE_HOME="$OUT/state"
XDG_CONFIG_HOME="$OUT/config"
XDG_RUNTIME_DIR="$OUT/run"
export XDG_STATE_HOME XDG_CONFIG_HOME XDG_RUNTIME_DIR
mkdir -p "$XDG_STATE_HOME" "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
unset SSH_AUTH_SOCK SSH_AGENT_PID
SOCKET="$OUT/sock"
DAEMON_PID=""
AGENT_PID=""
STRANGER_PID=""

# Everything is stopped by the pid this script was given, never by name: a name-matched kill would take a
# developer's own session down with it.
cleanup() {
    for pid in "$DAEMON_PID" "$AGENT_PID" "$STRANGER_PID"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$OUT"
}
trap cleanup EXIT

fail() {
    echo "error: $1" >&2
    for log in "$OUT/daemon.log" "$OUT/daemon.out" "$OUT/client.err"; do
        [ -s "$log" ] && { echo "--- $log ---" >&2; cat "$log" >&2; }
    done
    exit 1
}

# The pid an `ssh-agent -s` run printed.
agent_pid_of() {
    printf '%s\n' "$1" | sed -n 's/^SSH_AGENT_PID=\([0-9]*\);.*/\1/p'
}

# --- The fixture: one key in an agent, authorized by the daemon -------------------------------------
ssh-keygen -q -t ed25519 -N "" -C "e2e" -f "$OUT/key" || fail "ssh-keygen failed"
cp "$OUT/key.pub" "$OUT/authorized_keys"
chmod 600 "$OUT/authorized_keys"
AGENT_ENV="$(ssh-agent -s -a "$OUT/agent.sock")" || fail "ssh-agent did not start"
AGENT_PID="$(agent_pid_of "$AGENT_ENV")"
[ -n "$AGENT_PID" ] || fail "cannot read the agent's pid from: $AGENT_ENV"
SSH_AUTH_SOCK="$OUT/agent.sock" ssh-add -q "$OUT/key" 2> "$OUT/client.err" || fail "ssh-add failed"

# --- The daemon, on a port of the system's choosing -----------------------------------------------------
"$CONTOUR" daemon --socket "$SOCKET" --log-file "$OUT/daemon.log" \
    --listen-tcp 127.0.0.1:0 --auth keys --authorized-keys "$OUT/authorized_keys" \
    > "$OUT/daemon.out" 2>&1 &
DAEMON_PID=$!

# The daemon logs the port it was given, and the fingerprint of the identity it presents.
waited=0
LISTENING=""
while [ -z "$LISTENING" ]; do
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "the daemon exited before it listened"
    LISTENING="$(sed -n 's/.*native TCP listener on \([0-9.]*:[0-9]*\), TLS fingerprint \(SHA256:[^ ]*\).*/\1 \2/p' \
        "$OUT/daemon.log" 2> /dev/null | head -n 1)"
    waited=$((waited + 1))
    [ "$waited" -gt 200 ] && fail "the daemon did not listen within 20s"
    [ -n "$LISTENING" ] || sleep 0.1
done
HOSTPORT="${LISTENING% *}"
FINGERPRINT="${LISTENING#* }"
case "$HOSTPORT" in *:0) fail "the daemon logged port 0, not the one it was given: $LISTENING" ;; esac

# Standard input is not a terminal under ctest, so nothing here may be asked: a first contact is settled
# by --daemon-fingerprint or refused.
run_client() {
    timeout 60 "$CONTOUR" ls --daemon "$HOSTPORT" "$@" < /dev/null > "$OUT/client.out" 2> "$OUT/client.err"
}

# --- Nobody told the client whom to trust, so it is refused and pins nothing ---------------------------
if SSH_AUTH_SOCK="$OUT/agent.sock" run_client; then
    fail "a client that was not told the daemon's fingerprint unexpectedly connected"
fi
grep -q -- "--daemon-fingerprint" "$OUT/client.err" || fail "the refusal does not name --daemon-fingerprint"
[ ! -e "$XDG_CONFIG_HOME/contour/known_daemons" ] || fail "a refused first contact pinned the daemon"

if SSH_AUTH_SOCK="$OUT/agent.sock" run_client --daemon-fingerprint "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU"; then
    fail "a client given the wrong fingerprint unexpectedly connected"
fi
grep -q "not the" "$OUT/client.err" || fail "the wrong-fingerprint refusal does not say what was expected"

# --- With the fingerprint and the key, it signs in, and the daemon is pinned ----------------------------
SSH_AUTH_SOCK="$OUT/agent.sock" run_client --daemon-fingerprint "$FINGERPRINT" \
    || fail "a client with the fingerprint and an authorized key did not connect"
grep -q "$HOSTPORT" "$XDG_CONFIG_HOME/contour/known_daemons" || fail "the first contact did not pin the daemon"
grep -q "$FINGERPRINT" "$XDG_CONFIG_HOME/contour/known_daemons" || fail "the pin does not hold the fingerprint"

# --- From then on the pin is enough ---------------------------------------------------------------------
SSH_AUTH_SOCK="$OUT/agent.sock" run_client || fail "a pinned daemon was not connected to without the fingerprint"

# --- A key the daemon does not authorize gets no further ------------------------------------------------
ssh-keygen -q -t ed25519 -N "" -C "stranger" -f "$OUT/stranger" || fail "ssh-keygen failed"
STRANGER_ENV="$(ssh-agent -s -a "$OUT/stranger.sock")" || fail "the second ssh-agent did not start"
STRANGER_PID="$(agent_pid_of "$STRANGER_ENV")"
SSH_AUTH_SOCK="$OUT/stranger.sock" ssh-add -q "$OUT/stranger" 2>> "$OUT/client.err" || fail "ssh-add failed"
status=0
SSH_AUTH_SOCK="$OUT/stranger.sock" run_client || status=$?
[ "$status" -ne 0 ] || fail "a key the daemon does not authorize unexpectedly connected"
# Refused for the key, not for any other reason: the reply names the fingerprints that were offered.
grep -q "the daemon refused the connection" "$OUT/client.err" || fail "the stranger's refusal is not the daemon's"
grep -q "keys offered: SHA256:" "$OUT/client.err" || fail "the refusal does not name the offered key"

# --- forget-pin takes the pin away, and the next contact is a first one again ----------------------
"$CONTOUR" daemon forget-pin "$HOSTPORT" > "$OUT/forget.out" 2>&1 || fail "daemon forget-pin failed"
if SSH_AUTH_SOCK="$OUT/agent.sock" run_client; then
    fail "a forgotten daemon was connected to without its fingerprint"
fi

echo "tls key login: ok"
