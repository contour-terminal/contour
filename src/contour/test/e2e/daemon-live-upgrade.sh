#! /bin/sh
# Live-upgrade sweep for the contour_e2e_daemon_live_upgrade test: a real `contour daemon` replaces itself with
# a release (this very binary) while a hosted program runs, and nothing the program or its screen holds is
# lost. $1 is the contour binary.
#
# vthost_test drives the upgrade through fakes (a fake exec, in-memory sockets); only a real exec can show the
# wiring around it: the descriptors that cross it, the control connection's answer reaching the CLI although
# the daemon execs straight after sending it, the new image decoding what the old one encoded. This is that
# test, for a SAME-version upgrade -- the case every release pair a CI can build reduces to, and the one a
# "nothing to do" short-circuit would wrongly refuse.
#
# Scenario 1 hosts a program that prints its PID, transmits a kitty image (i=31), enters the alternate
# screen with SGR mouse tracking and prints a numbered line every 200 ms. After `contour daemon upgrade` it
# asserts that an exec happened (the daemon's upgrade generation went up, and the new image logged taking the
# session over), that the daemon and the program are the same processes (POSIX exec keeps the daemon's PID),
# that the counter is gapless across the handover, that the alternate screen, the mouse modes and the image
# survived (the program asks the terminal through DECRQM and a kitty placement, the terminal answers on the
# program's stdin), and that a profile setting that is not the default -- the terminal size, which the
# program reads back with `stty size` -- is the same afterwards.
#
# Scenario 2 breaks the configuration file the daemon started with and asks again: the new release cannot
# load it, so the upgrade is refused (exit 5, configUnreadable) and the old daemon serves on, untouched.
#
# Scenario 3 edits the configuration and upgrades again: a workspace opened after the upgrade has the edited
# profile's terminal size, and a further edit after it is reloaded by the upgraded image's own watcher.
#
# The handover pause is measured and printed, never asserted: how long the screen shows no new line.
#
# POSIX sh, and nothing a macOS or FreeBSD userland lacks -- no `timeout`, which neither ships: the pause is
# measured in milliseconds only where `date` knows GNU's %3N, in whole seconds elsewhere.

CONTOUR="$1"
[ -x "$CONTOUR" ] || { echo "usage: $0 /path/to/contour" >&2; exit 64; }

OUT="$(mktemp -d)"
SOCKET="$OUT/sock"
# The developer's own configuration and state stay out of it; and so does the pane this may be run from, whose
# CONTOUR_* variables would otherwise name it as the target of every verb below.
XDG_CONFIG_HOME="$OUT/config"
XDG_STATE_HOME="$OUT/state"
export XDG_CONFIG_HOME XDG_STATE_HOME
unset CONTOUR_WORKSPACE CONTOUR_TAB CONTOUR_PANE CONTOUR_MUX CONTOUR_DAEMON CONTOUR_PROFILE
CONFIG="$XDG_CONFIG_HOME/contour/contour.yml"
DAEMON_PID=""
PROGRAM_PID=""

daemon_pid_alive() { kill -0 "$DAEMON_PID" 2>/dev/null; }
daemon_pid_gone() { ! daemon_pid_alive; }

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

# Reaps the daemon, the shell's own child, giving it at most $1 deciseconds to end before it is killed: a
# daemon that hangs in its shutdown must not hold the RUN_SERIAL slot until ctest's own timeout.
reap_daemon() {
    wait_for "$1" daemon_pid_gone || kill -KILL "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
}

# Killed by PID, never by name: `pkill -f contour` on a developer's machine SIGKILLs their daily driver.
# $DAEMON_PID is the shell's own child, and the daemon keeps it across its exec; the program is the daemon's.
# Runs on every exit path, including the ones fail() takes.
cleanup() {
    [ -n "$PROGRAM_PID" ] && kill "$PROGRAM_PID" 2>/dev/null
    if [ -n "$DAEMON_PID" ] && daemon_pid_alive; then
        kill "$DAEMON_PID" 2>/dev/null
        reap_daemon 100
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

# Milliseconds since the epoch where `date` knows GNU's %3N; whole seconds elsewhere. A BSD or macOS `date`
# prints the unknown conversion as it is, or a %N without its width: anything but 13 digits is not
# milliseconds.
now_ms() {
    stamp="$(date +%s%3N)"
    case "$stamp" in
        *[!0-9]*) stamp="" ;;
    esac
    [ "${#stamp}" -eq 13 ] || stamp="$(date +%s)000"
    echo "$stamp"
}

# Runs the contour binary against the daemon under test, bounded: a verb that waits forever must not hold the
# RUN_SERIAL slot until ctest's own timeout. Bounded by a watchdog rather than `timeout`, which macOS and
# FreeBSD lack; the watchdog's own output goes nowhere, so that a sleep it leaves behind holds no pipe ctest
# reads. The output is in $OUT/last.txt, the status in $status.
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
program_started() { [ -s "$OUT/program.pid" ]; }
probe_answered() { [ -e "$OUT/probe.done" ]; }

# The tick numbers the program's screen shows now, one per line; the screen is captured to $OUT/screen.txt.
capture_ticks() {
    contour pane capture pane "$PANE" daemon "$SOCKET"
    [ "$status" -eq 0 ] || fail "pane capture exited $status"
    cp "$OUT/last.txt" "$OUT/screen.txt"
    sed -n 's/^ *tick \([0-9][0-9]*\) *$/\1/p' "$OUT/screen.txt" > "$OUT/ticks.txt"
}

last_tick() { tail -n 1 "$OUT/ticks.txt"; }

# The ticks on screen must form one unbroken run, at least $1 long: a byte lost across the handover would
# leave a hole in it. Prints nothing when they do, else the first offence.
ticks_gapless() {
    awk -v need="$1" 'NR > 1 && $1 != prev + 1 { print "tick " prev " is followed by tick " $1; bad = 1; exit }
                      { prev = $1 }
                      END { if (!bad && NR < need) print "only " NR " ticks on screen, wanted " need }' \
        "$OUT/ticks.txt"
}

# Asks the program to probe the terminal -- the sizes, the modes, the image -- and waits for its answers,
# which it writes to $OUT/probe-$1.*.
probe() {
    rm -f "$OUT/probe.done"
    printf '%s\n' "$1" > "$OUT/probe.tag"
    kill -USR1 "$PROGRAM_PID" 2>/dev/null || fail "the program is gone: it cannot be probed ($1)"
    wait_for 200 probe_answered || fail "the program did not answer the probe ($1) within 20s"
}

# --- The configuration the daemon starts with -----------------------------------------------------------------
#
# A profile setting that is not the default, and a shell nothing uses: the program below is named on the
# command line. 91x57 differs from the 80x24 default, and from what a client's workspace would impose; it is the
# size of the pty, which the program reads with `stty size`. 57 lines because the ticks must outlive the
# handover on the screen: the alternate screen keeps no history, and the run is checked for gaps there. At a
# tick every 200 ms that is over 11 s of them, however slowly a loaded machine upgrades.
mkdir -p "$XDG_CONFIG_HOME/contour"
cat > "$CONFIG" <<YAML
default_profile: main
profiles:
  main:
    shell: sh
    terminal_size:
      columns: 91
      lines: 57
YAML

# --- The hosted program ---------------------------------------------------------------------------------------
#
# Written to a file and run as `sh program.sh`, so that $$ is the PID of the process the daemon spawned.
cat > "$OUT/program.sh" <<'PROGRAM'
out="$1"
probe_tag=""

# Asks the terminal $2 (a control string) and writes what it answers on the program's stdin to $1, with the
# escape byte shown as ^. The tty is not canonical meanwhile, and gives up on a silent terminal after 2 s.
ask() {
    saved="$(stty -g)"
    stty -icanon -echo min 0 time 20
    printf "$2"
    dd bs=256 count=1 2>/dev/null | tr '\033' '^' > "$1"
    stty "$saved"
}

probe() {
    tag="$(cat "$out/probe.tag")"
    stty size > "$out/probe-$tag.size"
    # DECRQM: ESC [ ? Pm ; Ps $ y, Ps 1 for a mode that is set. 1049 is the alternate screen, 1000 mouse
    # tracking, 1006 its SGR encoding.
    ask "$out/probe-$tag.m1049" '\033[?1049$p'
    ask "$out/probe-$tag.m1000" '\033[?1000$p'
    ask "$out/probe-$tag.m1006" '\033[?1006$p'
    # A placement of the image transmitted at the start: answered OK if the terminal still holds image 31.
    ask "$out/probe-$tag.image" '\033_Gi=31,a=p;\033\\'
    : > "$out/probe.done"
}

trap 'probe_tag=1' USR1
echo $$ > "$out/program.pid.tmp"
mv "$out/program.pid.tmp" "$out/program.pid"
echo "pid $$"
# The alternate screen, SGR mouse tracking.
printf '\033[?1049h\033[?1000h\033[?1006h'
# The image belongs to the screen it is sent on: this one.
# 1x1 RGB pixel, transmitted and kept, quietly: its reply would wait unread on the program's stdin.
printf '\033_Gi=31,f=24,s=1,v=1,a=t,q=2;AAAA\033\\'
n=0
while :; do
    n=$((n + 1))
    echo "tick $n"
    [ -n "$probe_tag" ] && { probe_tag=""; probe; }
    sleep 0.2
done
PROGRAM

# --- A daemon that loaded its configuration file --------------------------------------------------------------
"$CONTOUR" daemon --socket "$SOCKET" --log-file "$OUT/daemon.log" > "$OUT/daemon.out" 2>&1 &
DAEMON_PID=$!
wait_for 200 socket_bound || fail "the daemon did not bind $SOCKET within 20s"
daemon_pid_alive || fail "the daemon exited before serving"

contour new daemon "$SOCKET" json sh "$OUT/program.sh" "$OUT"
[ "$status" -eq 0 ] || fail "contour new exited $status"
PANE="$(sed -n 's/.*"pane":"\([^"]*\)".*/\1/p' "$OUT/last.txt")"
[ -n "$PANE" ] || fail "contour new named no pane"
wait_for 200 program_started || fail "the hosted program did not start within 20s"
PROGRAM_PID="$(cat "$OUT/program.pid")"

# The ticks run, and the baseline probe is what an unchanged terminal answers: the same probe after the
# upgrade is only evidence if it can pass before it.
capture_ticks
waited=0
while [ "$(wc -l < "$OUT/ticks.txt")" -lt 10 ]; do
    waited=$((waited + 1))
    [ "$waited" -gt 100 ] && fail "the program printed no ticks within 20s"
    sleep 0.2
    capture_ticks
done
probe before
grep -qF '^[?1049;1$y' "$OUT/probe-before.m1049" || fail "baseline: the alternate screen is not set: $(cat "$OUT/probe-before.m1049")"
grep -qF '^[?1000;1$y' "$OUT/probe-before.m1000" || fail "baseline: mouse tracking is not set: $(cat "$OUT/probe-before.m1000")"
grep -qF '^[?1006;1$y' "$OUT/probe-before.m1006" || fail "baseline: SGR mouse encoding is not set: $(cat "$OUT/probe-before.m1006")"
grep -qF 'i=31;OK' "$OUT/probe-before.image" || fail "baseline: image 31 is not placed: $(cat "$OUT/probe-before.image")"
[ "$(tr -d ' \n' < "$OUT/probe-before.size")" = "5791" ] \
    || fail "baseline: the pty is '$(cat "$OUT/probe-before.size")', not the profile's 57 91"

contour ls daemon "$SOCKET" json
PANES_BEFORE="$(cat "$OUT/last.txt")"
contour daemon status --socket "$SOCKET"
[ "$status" -eq 0 ] || fail "daemon status exited $status"
grep -qF "(pid $DAEMON_PID)" "$OUT/last.txt" || fail "daemon status does not name the daemon's pid $DAEMON_PID"
grep -qE '^sessions: +1$' "$OUT/last.txt" || fail "the daemon does not host exactly the one session"
grep -qE '^upgrades: +0$' "$OUT/last.txt" || fail "a fresh daemon does not count 0 upgrades"

# --- Scenario 1: a same-version upgrade -----------------------------------------------------------------------
# The last tick before the upgrade, taken right before it: the one that must still be on screen after.
capture_ticks
TICK_BEFORE="$(last_tick)"
T0="$(now_ms)"
contour daemon upgrade --socket "$SOCKET"
T1="$(now_ms)"
[ "$status" -eq 0 ] || fail "contour daemon upgrade exited $status, not 0 (a same-version upgrade is accepted)"
grep -q "upgraded" "$OUT/last.txt" || fail "contour daemon upgrade does not say it upgraded"

# The pause: until the screen shows a line it did not show before the upgrade was asked for. A line was
# printed every 200 ms meanwhile; they wait in the pty until the new image reads them.
seen=""
waited=0
while :; do
    capture_ticks
    [ "$(last_tick)" -gt "$TICK_BEFORE" ] 2>/dev/null && break
    waited=$((waited + 1))
    [ "$waited" -gt 300 ] && fail "no new tick appeared within 30s of the upgrade"
    sleep 0.05
done
T2="$(now_ms)"
echo "live upgrade: the command took $((T1 - T0)) ms; the first new tick was on screen $((T2 - T0)) ms after it was asked for"

# What the daemon itself says about the pause, from its log: the announcement to the first listener serving.
grep -a -e "live upgrade to " -e "serving on" "$OUT/daemon.log" | tail -n 2 | cut -c2-27 | tr '\n' ' ' \
    | sed 's/^/live upgrade: the daemon logged the upgrade at, and served again at: /'
echo
daemon_pid_alive || fail "the daemon died in the upgrade"
contour daemon status --socket "$SOCKET"
[ "$status" -eq 0 ] || fail "daemon status after the upgrade exited $status"
grep -qF "(pid $DAEMON_PID)" "$OUT/last.txt" || fail "the daemon's pid is not $DAEMON_PID any more"
grep -qE '^sessions: +1$' "$OUT/last.txt" || fail "the upgraded daemon does not host exactly the one session"
# An exec happened, not a rollback that kept the same process serving: only a new image counts one upgrade
# more, and only a new image takes the session over from a handover.
grep -qE '^upgrades: +1$' "$OUT/last.txt" || fail "the daemon does not count the upgrade: no new image serves"
awk '/live upgrade to / { announced = 1 } announced && /took over 1 of 1 session\(s\) from release / { found = 1 }
     END { exit !found }' "$OUT/daemon.log" \
    || fail "the daemon log has no handover taken over after the upgrade was announced"
kill -0 "$PROGRAM_PID" 2>/dev/null || fail "the hosted program $PROGRAM_PID died in the upgrade"
[ "$(cat "$OUT/program.pid")" = "$PROGRAM_PID" ] || fail "the program pid file changed"
contour ls daemon "$SOCKET" json
PANES_AFTER="$(cat "$OUT/last.txt")"
[ "$PANES_BEFORE" = "$PANES_AFTER" ] || fail "the workspaces, tabs and panes differ after the upgrade: $PANES_BEFORE / $PANES_AFTER"

# Still counting, and no hole where the handover was: the run on screen reaches from before the upgrade to
# after it, in single steps.
counted_on() { capture_ticks; [ "$(last_tick)" -ge "$(($1 + 10))" ] 2>/dev/null; }
wait_for 300 counted_on "$TICK_BEFORE" || fail "the program did not count 10 more ticks within 30s of the upgrade"
grep -qx "$TICK_BEFORE" "$OUT/ticks.txt" || fail "tick $TICK_BEFORE, the last before the upgrade, is not on screen any more"
gap="$(ticks_gapless 20)"
[ -z "$gap" ] || fail "the counter is not gapless across the handover: $gap"

# What the terminal held: the same answers as before.
probe after
for item in m1049 m1000 m1006 image; do
    cmp -s "$OUT/probe-before.$item" "$OUT/probe-after.$item" \
        || fail "the terminal answered '$(cat "$OUT/probe-after.$item")' to the $item probe after the upgrade, '$(cat "$OUT/probe-before.$item")' before"
done
cmp -s "$OUT/probe-before.size" "$OUT/probe-after.size" \
    || fail "the profile's terminal size is '$(cat "$OUT/probe-after.size")' after the upgrade, '$(cat "$OUT/probe-before.size")' before"

# --- Scenario 2: a configuration the new release cannot load ---------------------------------------------------
#
# The daemon read a valid file at its start; the file now is not YAML. Asking the upgrade's readiness probe
# to load it fails, and nothing may have been stopped by then: exit status 5, the refusal named, the same
# daemon and the same program, still counting.
cp "$CONFIG" "$OUT/contour.yml.good"
printf 'profiles: [\n' > "$CONFIG"
capture_ticks
TICK_REFUSED="$(last_tick)"
contour daemon upgrade --socket "$SOCKET"
[ "$status" -eq 5 ] || fail "an upgrade over a broken configuration exited $status, not 5"
grep -q "cannot load the daemon's configuration" "$OUT/last.txt" \
    || fail "the refusal does not name the configuration"
cp "$OUT/contour.yml.good" "$CONFIG"

daemon_pid_alive || fail "the refused upgrade killed the daemon"
kill -0 "$PROGRAM_PID" 2>/dev/null || fail "the refused upgrade killed the hosted program"
contour daemon status --socket "$SOCKET"
[ "$status" -eq 0 ] || fail "daemon status after the refusal exited $status"
grep -qF "(pid $DAEMON_PID)" "$OUT/last.txt" || fail "the refused upgrade changed the daemon's pid"
# Refused before anything was announced: no upgrade counted, and no failure recorded for a later reader.
grep -qE '^upgrades: +1$' "$OUT/last.txt" || fail "the refused upgrade changed the upgrade count"
grep -q '^last upgrade:' "$OUT/last.txt" && fail "a refusal before the announcement was recorded as a failed upgrade"
wait_for 300 counted_on "$TICK_REFUSED" || fail "the program stopped counting after the refused upgrade"
gap="$(ticks_gapless 20)"
[ -z "$gap" ] || fail "the counter has a hole after the refused upgrade: $gap"

# --- Scenario 3: an edited configuration, carried over and reloaded ------------------------------------------
#
# A workspace a control connection opens with no client attached is spawned at the profile's terminal size as
# the daemon read it at its start: the size of a new workspace shows which configuration the upgraded image
# started on. Its reload, a poll of the file every 2 s, shows in its log.
cat > "$OUT/size.sh" <<'SIZE'
stty size > "$1.tmp"
mv "$1.tmp" "$1"
exec sleep 600
SIZE

# Sets the profile's terminal size to $1 columns by $2 lines. A second later than the last write, so that the
# file's modification time moves on a file system that keeps whole seconds.
set_profile_size() {
    sleep 1
    sed -e "s/columns: .*/columns: $1/" -e "s/lines: .*/lines: $2/" "$OUT/contour.yml.good" > "$CONFIG"
}

reloads() { grep -c 'contour.yml: configuration reloaded' "$OUT/daemon.log"; }
reloaded_since() { [ "$(reloads)" -gt "$1" ]; }
size_written() { [ -s "$OUT/size-$1" ]; }

set_profile_size 83 33
contour daemon upgrade --socket "$SOCKET"
[ "$status" -eq 0 ] || fail "the upgrade over the restored configuration exited $status, not 0"
daemon_pid_alive || fail "the daemon died in the second upgrade"
kill -0 "$PROGRAM_PID" 2>/dev/null || fail "the hosted program died in the second upgrade"
contour daemon status --socket "$SOCKET"
grep -qE '^upgrades: +2$' "$OUT/last.txt" || fail "the second upgrade did not count"
contour new new-window daemon "$SOCKET" json sh "$OUT/size.sh" "$OUT/size-carried"
[ "$status" -eq 0 ] || fail "contour new new-window exited $status"
wait_for 200 size_written carried || fail "the new workspace did not report its size within 20s"
[ "$(tr -d ' \n' < "$OUT/size-carried")" = "3383" ] \
    || fail "a workspace after the upgrade is '$(cat "$OUT/size-carried")', not the edited profile's 33 83"

# After the upgrade: only the upgraded image's own watcher can reload this edit.
RELOADS="$(reloads)"
set_profile_size 77 29
wait_for 100 reloaded_since "$RELOADS" || fail "the upgraded daemon did not reload an edited configuration within 10s"

# --- Stopping it ends the program with it ----------------------------------------------------------------------
contour daemon stop --socket "$SOCKET" --yes
[ "$status" -eq 0 ] || fail "daemon stop exited $status"
wait_for 100 daemon_pid_gone || fail "the daemon was still running 10s after daemon stop"
reap_daemon 0
DAEMON_PID=""
echo "live upgrade: ok"
exit 0
