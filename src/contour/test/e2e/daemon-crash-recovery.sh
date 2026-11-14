#! /bin/sh
# Crash recovery for the contour_e2e_daemon_crash_recovery test: a real `contour daemon` -- its PTY keeper and
# the daemon the keeper keeps -- loses the daemon to SIGKILL twice, its configuration broken in between, and
# the hosted program never notices. $1 is the contour binary.
#
# vthost_test kills a daemon behind a keeper out of the test binary itself; only the real verb shows that a
# daemon its keeper restarts tolerates a configuration that no longer loads (it would hang up every program
# it is to recover if it refused), and that the CLI finds the same workspaces, tabs and panes afterwards.
#
# POSIX sh, and nothing a macOS or FreeBSD userland lacks.

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

OUT="$(mktemp -d)"
SOCKET="$OUT/sock"
XDG_CONFIG_HOME="$OUT/config"
XDG_STATE_HOME="$OUT/state"
export XDG_CONFIG_HOME XDG_STATE_HOME
unset CONTOUR_WORKSPACE CONTOUR_TAB CONTOUR_PANE CONTOUR_MUX CONTOUR_DAEMON CONTOUR_PROFILE
CONFIG="$XDG_CONFIG_HOME/contour/contour.yml"
KEEPER_PID=""
PROGRAM_PID=""

keeper_alive() { kill -0 "$KEEPER_PID" 2>/dev/null; }
keeper_gone() { ! keeper_alive; }
# The daemon's own pid, as the last `daemon status` names it.
serving_pid() { sed -n 's/^daemon: *running (pid \([0-9]*\))$/\1/p' "$OUT/last.txt"; }

# Waits, at most $1 deciseconds, for the command $2... to succeed.
wait_for() {
    limit="$1"
    shift
    waited=0
    until "$@"; do
        waited=$((waited + 1))
        [ "$waited" -gt "$limit" ] && return 1
        sleep 0.1
    done
}

# Killed by PID, never by name: `pkill -f contour` on a developer's machine SIGKILLs their daily driver.
cleanup() {
    [ -n "$PROGRAM_PID" ] && kill "$PROGRAM_PID" 2>/dev/null
    if [ -n "$KEEPER_PID" ] && keeper_alive; then
        kill "$KEEPER_PID" 2>/dev/null
        wait_for 150 keeper_gone || kill -KILL "$KEEPER_PID" 2>/dev/null
        wait "$KEEPER_PID" 2>/dev/null
    fi
    rm -rf "$OUT"
}
trap cleanup EXIT

fail() {
    echo "error: $1" >&2
    [ -s "$OUT/daemon.log" ] && { echo "--- daemon log ---" >&2; tail -n 40 "$OUT/daemon.log" >&2; }
    [ -s "$OUT/last.txt" ] && { echo "--- last command output ---" >&2; cat "$OUT/last.txt" >&2; }
    exit 1
}

# Runs the contour binary against the daemon under test, bounded by a watchdog (no `timeout` on macOS). The
# output is in $OUT/last.txt, the status in $status.
contour() {
    status=0
    "$CONTOUR" "$@" > "$OUT/last.txt" 2>&1 < /dev/null &
    verb=$!
    (sleep 60; kill -KILL "$verb") > /dev/null 2>&1 &
    watchdog=$!
    wait "$verb" || status=$?
    kill "$watchdog" 2>/dev/null
    wait "$watchdog" 2>/dev/null
}

socket_bound() { [ -e "$SOCKET" ]; }
# The last `contour ls`, the panes' sizes included: a recovered pane keeps the size its program runs at.
listing() { cat "$OUT/last.txt"; }
program_started() { [ -s "$OUT/program.pid" ]; }
redrawn() { [ -s "$OUT/winched" ]; }

# Asks the daemon for its status until a daemon other than $1 answers.
new_daemon_serves() {
    contour daemon status --socket "$SOCKET"
    [ "$status" -eq 0 ] && [ -n "$(serving_pid)" ] && [ "$(serving_pid)" != "$1" ]
}

mkdir -p "$XDG_CONFIG_HOME/contour"
cat > "$CONFIG" <<YAML
default_profile: main
profiles:
  main:
    shell: sh
YAML

# A program that prints its pid, and notes each SIGWINCH: a restarted daemon asks it to redraw.
cat > "$OUT/program.sh" <<'PROGRAM'
out="$1"
trap 'echo w >> "$out/winched"' WINCH
echo $$ > "$out/program.pid.tmp"
mv "$out/program.pid.tmp" "$out/program.pid"
while :; do sleep 0.1; done
PROGRAM

"$CONTOUR" daemon --socket "$SOCKET" --log-file "$OUT/daemon.log" > "$OUT/daemon.out" 2>&1 &
KEEPER_PID=$!
wait_for 200 socket_bound || fail "the daemon did not bind $SOCKET within 20s"

contour new daemon "$SOCKET" json sh "$OUT/program.sh" "$OUT"
[ "$status" -eq 0 ] || fail "contour new exited $status"
wait_for 200 program_started || fail "the hosted program did not start within 20s"
PROGRAM_PID="$(cat "$OUT/program.pid")"

contour ls daemon "$SOCKET" json
PANES_BEFORE="$(listing)"
contour daemon status --socket "$SOCKET"
[ "$status" -eq 0 ] || fail "daemon status exited $status"
SERVING_PID="$(serving_pid)"
[ -n "$SERVING_PID" ] || fail "daemon status names no daemon"

# Twice, the second time within the crash loop's window: both restarts run on a configuration that does not
# load, and neither ends the program.
printf 'default_profile: [this is not\n  valid: yaml\n' > "$CONFIG"
for crash in 1 2; do
    rm -f "$OUT/winched"
    kill -KILL "$SERVING_PID" || fail "crash $crash: the daemon $SERVING_PID is gone already"
    wait_for 600 new_daemon_serves "$SERVING_PID" || fail "crash $crash: no daemon served again within 60s"
    SERVING_PID="$(serving_pid)"
    keeper_alive || fail "crash $crash: the keeper $KEEPER_PID ended"
    [ "$(ps -o ppid= -p "$SERVING_PID" | tr -d ' ')" = "$KEEPER_PID" ] \
        || fail "crash $crash: the daemon $SERVING_PID is not the keeper's child"
    grep -qE '^sessions: +1$' "$OUT/last.txt" || fail "crash $crash: the daemon does not host the one session"
    kill -0 "$PROGRAM_PID" 2>/dev/null || fail "crash $crash: the hosted program $PROGRAM_PID died"
    wait_for 100 redrawn || fail "crash $crash: the program was not asked to redraw"
    contour ls daemon "$SOCKET" json
    [ "$(listing)" = "$PANES_BEFORE" ] \
        || fail "crash $crash: the workspaces, tabs and panes differ: $PANES_BEFORE / $(listing)"
done

grep -q "could not be loaded" "$OUT/daemon.log" \
    || fail "the restarted daemon does not say it runs on its built-in configuration"
echo "crash recovery: two crashes, the configuration broken, the program and its pane intact"
