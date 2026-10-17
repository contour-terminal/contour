#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Proves that an out-of-memory kill inside a session cannot stop the process hosting it.

Runs session_oom_driver as a transient systemd user unit with OOMPolicy=stop -- the policy desktops
give the unit Contour runs in -- three times:

  --placement=none     the hog shares the driver's unit, so the unit must end in Result=oom-kill.
                       This proves the test sees the very failure it guards against.
  --placement=systemd  the hog runs in a contour-session-*.scope of its own, so the unit must end
                       in Result=success.
  --report-placements  checks the answer, not only the outcome: systemd moves a child into its scope
                       whether or not its answer reaches Contour, and a placement that does not hear it
                       reports a timeout and holds off placing the sessions after it. It places a first
                       and a later child through fresh placements, and must see every request answered
                       as placed. Its scopes carry a prefix unique to this run; any of them left over is
                       stopped, and fails the test.

Exits 77, CTest's skip code, when there is no systemd user manager to run under -- unless
CONTOUR_REQUIRE_SYSTEMD_TESTS=1, which makes that a failure.
"""

import os
import re
import subprocess
import sys
import uuid

SKIP = 77
UNIT_MEMORY_MAX = "512M"


def cases(scope_prefix: str) -> tuple[tuple[str, str], ...]:
    """The driver arguments to run, and the Result each unit must end in."""
    return (
        ("--placement=none", "oom-kill"),
        ("--placement=systemd", "success"),
        (f"--report-placements={scope_prefix}", "success"),
    )


def user_manager_available() -> bool:
    try:
        probe = subprocess.run(
            ["systemd-run", "--user", "--wait", "--quiet", "--collect", "true"],
            capture_output=True,
            timeout=30,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False
    return probe.returncode == 0


def leftover_units(prefix: str) -> list[str]:
    """The units whose name starts with prefix, loaded in any state."""
    listed = subprocess.run(
        ["systemctl", "--user", "list-units", "--all", "--plain", "--no-legend", f"{prefix}-*"],
        capture_output=True,
        text=True,
    ).stdout
    return [line.split()[0] for line in listed.splitlines() if line.strip()]


def run(driver: str, argument: str) -> tuple[str, str]:
    """Runs the driver as a unit. Returns the unit's Result, and what to show if it was wrong."""
    unit = f"contour-oom-isolation-test-{uuid.uuid4().hex[:12]}"
    completed = subprocess.run(
        [
            "systemd-run", "--user", "--wait", "--collect", f"--unit={unit}",
            "--property=OOMPolicy=stop",
            f"--property=MemoryMax={UNIT_MEMORY_MAX}",
            "--property=MemorySwapMax=0",
            driver, argument,
        ],
        capture_output=True,
        text=True,
        timeout=180,
    )
    summary = completed.stdout + completed.stderr
    match = re.search(r"Finished with result: (\S+)", summary)
    journal = subprocess.run(
        ["journalctl", "--user", f"--unit={unit}", "--no-pager", "--output=cat"],
        capture_output=True,
        text=True,
    ).stdout
    return (match.group(1) if match else "unknown"), summary + journal


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <session_oom_driver>", file=sys.stderr)
        return 2
    if not user_manager_available():
        print("no systemd user manager to run under", file=sys.stderr)
        return 1 if os.environ.get("CONTOUR_REQUIRE_SYSTEMD_TESTS") == "1" else SKIP

    failed = False
    scope_prefix = f"contour-placement-test-{uuid.uuid4().hex[:12]}"
    for argument, expected in cases(scope_prefix):
        result, details = run(sys.argv[1], argument)
        print(f"{argument}: Result={result} (expected {expected})")
        if result != expected:
            failed = True
            print(details)

    # The children are gone, so their scopes should be too; stop any that is not.
    leftovers = leftover_units(scope_prefix)
    if leftovers:
        failed = True
        print(f"scopes left over: {' '.join(leftovers)}")
        subprocess.run(["systemctl", "--user", "stop", *leftovers], capture_output=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
