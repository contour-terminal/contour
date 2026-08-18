#! /usr/bin/env bash
# Asserts that a wedged desktop-notification service does not stall Contour -- the regression behind
# issue #2051, where starting Contour with dunst disabled took about two minutes.
#
# The setup reproduces "disabled notification daemon" the way a real desktop presents it: a private
# session bus on which org.freedesktop.Notifications is ACTIVATABLE (a .service file names it) but
# whose activation never claims the name. Every synchronous D-Bus call towards it therefore sits
# until the reply timeout expires -- Qt's default is 25 s -- and returns NoReply, which is exactly
# the error the issue reports.
#
# org.freedesktop.portal.Desktop is wedged the same way, because a sandboxed Contour talks to the
# notification portal instead and inherits the same requirement not to wait.
#
# Usage: notification-nonblocking.sh <path-to-contour_gui_test>

set -euo pipefail

readonly TEST_BINARY="${1:?usage: notification-nonblocking.sh <path-to-contour_gui_test>}"

# One D-Bus reply timeout is 25 s, so a budget below that fails on the FIRST blocking call rather
# than needing several to accumulate. The notification cases themselves run in well under a second.
readonly MAX_SECONDS=20

readonly WORK_DIR="$(mktemp -d)"
bus_pid=""

cleanup() {
    [[ -n "$bus_pid" ]] && kill "$bus_pid" 2>/dev/null
    rm -rf "$WORK_DIR"
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK_DIR/services"

# Activatable, but /bin/sleep never claims the name: the bus waits for an owner that never arrives.
cat >"$WORK_DIR/services/org.freedesktop.Notifications.service" <<EOF
[D-BUS Service]
Name=org.freedesktop.Notifications
Exec=/bin/sleep 600
EOF

# The same trap, set for the portal: a sandboxed Contour speaks org.freedesktop.portal.Notification
# instead, and it must be no more willing to wait than the session-bus backend is. A desktop whose
# xdg-desktop-portal is installed but not answering is the same failure, one bus name over.
# @see issue #2074.
cat >"$WORK_DIR/services/org.freedesktop.portal.Desktop.service" <<EOF
[D-BUS Service]
Name=org.freedesktop.portal.Desktop
Exec=/bin/sleep 600
EOF

cat >"$WORK_DIR/session.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus-busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <servicedir>$WORK_DIR/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF

# --print-address/--print-pid write to the given descriptors, which is the only way to read both.
dbus-daemon --config-file="$WORK_DIR/session.conf" --fork \
    --print-address=3 --print-pid=4 \
    3>"$WORK_DIR/address" 4>"$WORK_DIR/pid"

bus_address="$(cat "$WORK_DIR/address")"
bus_pid="$(cat "$WORK_DIR/pid")"

echo "notification-nonblocking: wedged bus at $bus_address (pid $bus_pid)"

# SECONDS is bash's own wall-clock counter, so no date(1) parsing and no subshell.
SECONDS=0
set +e
# CONTOUR_TEST_NOTIFICATION_SEND opts the "real D-Bus transport" case into actually sending. It is
# off by default so a developer running the suite on their own desktop is not shown a stray
# notification; here nothing is listening, so there is nothing to show.
DBUS_SESSION_BUS_ADDRESS="$bus_address" CONTOUR_TEST_NOTIFICATION_SEND=1 \
    "$TEST_BINARY" "[notification]"
readonly test_status=$?
set -e
readonly elapsed=$SECONDS

if [[ $test_status -ne 0 ]]; then
    echo "notification-nonblocking: FAILED -- the notification tests did not pass (exit $test_status)"
    exit "$test_status"
fi

if [[ $elapsed -ge $MAX_SECONDS ]]; then
    echo "notification-nonblocking: FAILED -- took ${elapsed}s against an unresponsive notification"
    echo "  daemon, budget is ${MAX_SECONDS}s. A D-Bus call is being made synchronously; see #2051."
    exit 1
fi

echo "notification-nonblocking: OK -- ${elapsed}s against an unresponsive notification daemon"
exit 0
