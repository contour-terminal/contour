#! /bin/sh
# Hosting sweep for the contour_e2e_hosting test: where a GUI launch's sessions run, as `daemon.mode`
# decides it (cli::decideHosting). $1 is the contour binary.
#
# The table itself is unit-tested (contour_test "[hosting]"); what only a real launch can show is the
# wiring around it: that `required` ends the launch before any window exists, that `auto` comes up
# in-process instead -- saying why -- and that `contour client --window` names a window the daemon lacks.
#
# The daemon this configuration names can never start: its socket lies below a regular file, so the
# directory it needs cannot be made. Every launch below spawns one on demand, and gives up after the
# spawn's 5 s wait.

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

OUT="$(mktemp -d)"
DAEMON_PID=""
# The daemon is killed by its PID, never by name, which would reach a developer's own terminal too.
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
    [ -s "$2" ] && cat "$2" >&2
    exit 1
}

# The developer's own configuration stays out of it, and nothing staged lands in their state directory.
XDG_CONFIG_HOME="$OUT/config"
XDG_STATE_HOME="$OUT/state"
export XDG_CONFIG_HOME XDG_STATE_HOME
mkdir -p "$XDG_CONFIG_HOME/contour"
touch "$OUT/blocker"
# The profile's shell exits at once, so a launch that comes up in-process ends by itself. It is the profile's,
# not a program named on the command line -- those below name /bin/sh, which differs from it, as a named program
# must to count as one (ContourGuiApp::cliCommand) -- and a launch naming one is never attached.
cat > "$XDG_CONFIG_HOME/contour/contour.yml" <<YAML
default_profile: main
daemon:
  socket: $OUT/blocker/run/sock
profiles:
  main:
    shell: sh
    arguments: ["-c", "exit 0"]
YAML

# `timeout` bounds a regression into a launch that waits forever, which would otherwise hold the
# RUN_SERIAL slot until ctest's own TIMEOUT.

# --- required: no daemon, no window ---------------------------------------------------------------
timeout 60 "$CONTOUR" terminal daemon-mode required early-exit-threshold 0 > "$OUT/required.log" 2>&1
status=$?
[ "$status" -eq 1 ] || fail "required: expected exit status 1, got $status" "$OUT/required.log"
grep -q "^contour: .*(unix:$OUT/blocker/run/sock)" "$OUT/required.log" \
    || fail "required: the launch printed no reason naming the daemon it wanted" "$OUT/required.log"

# --- auto: in-process instead, and said so ----------------------------------------------------------
timeout 60 "$CONTOUR" terminal daemon-mode auto early-exit-threshold 0 > "$OUT/auto.log" 2>&1
status=$?
[ "$status" -eq 0 ] || fail "auto: expected the in-process shell's exit status 0, got $status" "$OUT/auto.log"
grep -q "Hosting this window's sessions in-process: .*(unix:$OUT/blocker/run/sock)" "$OUT/auto.log" \
    || fail "auto: the fallback to in-process hosting was not reported" "$OUT/auto.log"

# --- a program named on the command line: in-process under auto, refused under required -------------
timeout 60 "$CONTOUR" terminal daemon-mode auto early-exit-threshold 0 execute /bin/sh -c "exit 0" \
    > "$OUT/named-auto.log" 2>&1
status=$?
[ "$status" -eq 0 ] || fail "auto with a program: expected exit status 0, got $status" "$OUT/named-auto.log"
if grep -q "Hosting this window's sessions in-process" "$OUT/named-auto.log"; then
    fail "auto with a program: it tried a daemon it can never be attached to" "$OUT/named-auto.log"
fi
timeout 60 "$CONTOUR" terminal daemon-mode required early-exit-threshold 0 execute /bin/sh -c "exit 0" \
    > "$OUT/named-required.log" 2>&1
status=$?
[ "$status" -eq 1 ] || fail "required with a program: expected exit status 1, got $status" "$OUT/named-required.log"
grep -q "^contour: daemon.mode is required, but a program given on the command line cannot be attached" \
    "$OUT/named-required.log" || fail "required with a program: no refusal printed" "$OUT/named-required.log"

# --- a mode nobody has --------------------------------------------------------------------------------
timeout 60 "$CONTOUR" terminal daemon-mode sometimes > "$OUT/unknown.log" 2>&1
status=$?
[ "$status" -eq 1 ] || fail "unknown mode: expected exit status 1, got $status" "$OUT/unknown.log"
grep -q "^contour: unknown --daemon-mode 'sometimes'" "$OUT/unknown.log" \
    || fail "unknown mode: the rejection does not name the option" "$OUT/unknown.log"

# --- contour client --window: a window the daemon does not have ------------------------------------
mkdir -m 700 "$OUT/run"
"$CONTOUR" daemon --socket "$OUT/run/sock" > "$OUT/daemon.log" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 100); do
    [ -S "$OUT/run/sock" ] && break
    sleep 0.1
done
[ -S "$OUT/run/sock" ] || fail "the daemon never bound its socket" "$OUT/daemon.log"
timeout 60 "$CONTOUR" client --socket "$OUT/run/sock" --window 99 > "$OUT/window.log" 2>&1
status=$?
[ "$status" -eq 1 ] || fail "--window: expected exit status 1, got $status" "$OUT/window.log"
grep -q "^contour client: the daemon has no window 99$" "$OUT/window.log" \
    || fail "--window: the client did not name the missing window" "$OUT/window.log"

# Nothing Qt reports at teardown follows any of the launches.
for log in required auto named-auto named-required unknown window; do
    if grep -qiE "QApplication|destroyed while|still in use|QObject::|ASSERT" "$OUT/$log.log"; then
        fail "the $log launch warned at teardown:" "$OUT/$log.log"
    fi
done
exit 0
