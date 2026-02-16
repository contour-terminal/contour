#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Interactive test script for the Kitty OSC 99 desktop notification protocol.

Exercises all facets of the OSC 99 implementation in Contour:
  - Simple title-only notifications
  - Title + body notifications
  - Urgency levels (low, normal, critical)
  - Display occasion filtering (always, unfocused, invisible)
  - Auto-close timeout
  - Application name override
  - Base64-encoded payloads
  - Chunked (multi-part) payloads
  - Notification replacement (same identifier)
  - Close request
  - Alive query
  - Capability query
  - Activation flags (focus, report, both)
  - Close event reporting

Usage:
    python3 test/osc99-notifications.py              # run all tests interactively
    python3 test/osc99-notifications.py --list        # list available tests
    python3 test/osc99-notifications.py --test title  # run a specific test
    python3 test/osc99-notifications.py --test title --test urgency  # run multiple
    python3 test/osc99-notifications.py --delay 3     # 3s pause between tests
    python3 test/osc99-notifications.py --id myapp    # custom identifier prefix
    python3 test/osc99-notifications.py --batch       # non-interactive, no prompts
"""

import argparse
import base64
import sys
import time

# -- ANSI helpers ----------------------------------------------------------

ESC = "\033"
OSC = f"{ESC}]"
ST = f"{ESC}\\"
BOLD = f"{ESC}[1m"
DIM = f"{ESC}[2m"
RESET = f"{ESC}[0m"
CYAN = f"{ESC}[36m"
GREEN = f"{ESC}[32m"
YELLOW = f"{ESC}[33m"
RED = f"{ESC}[31m"
MAGENTA = f"{ESC}[35m"


def osc99(metadata: str, payload: str = "") -> str:
    """Build a raw OSC 99 escape sequence string."""
    return f"{OSC}99;{metadata};{payload}{ST}"


def send(sequence: str) -> None:
    """Write a raw escape sequence to stdout."""
    sys.stdout.write(sequence)
    sys.stdout.flush()


def heading(title: str) -> None:
    """Print a section heading."""
    print(f"\n{BOLD}{CYAN}{'=' * 60}{RESET}")
    print(f"{BOLD}{CYAN}  {title}{RESET}")
    print(f"{BOLD}{CYAN}{'=' * 60}{RESET}\n")


def info(msg: str) -> None:
    """Print an informational message."""
    print(f"  {DIM}{msg}{RESET}")


def show_sequence(label: str, seq: str) -> None:
    """Display a human-readable representation of an escape sequence."""
    readable = seq.replace(ESC, "<ESC>").replace("\a", "<BEL>")
    print(f"  {YELLOW}{label}:{RESET} {DIM}{readable}{RESET}")


def wait(args: argparse.Namespace) -> None:
    """Pause between tests: either prompt for Enter or sleep."""
    if args.batch:
        time.sleep(args.delay)
    else:
        input(f"\n  {DIM}Press Enter to continue...{RESET}")


# -- Individual test functions ---------------------------------------------


def test_title(args: argparse.Namespace) -> None:
    """Simple title-only notification."""
    heading("Test: Simple Title Notification")
    info("Sends a notification with only a title (default payload type).")
    info("Expected: a desktop notification with title 'Hello from Contour!'")

    identifier = f"{args.id}-title"
    seq = osc99(f"i={identifier}", "Hello from Contour!")
    show_sequence("Sequence", seq)
    send(seq)
    wait(args)


def test_title_body(args: argparse.Namespace) -> None:
    """Title + body notification (two separate sequences)."""
    heading("Test: Title + Body Notification")
    info("Sends a title first, then a body in a second sequence.")
    info("Expected: notification with title 'Build Complete' and body text.")

    identifier = f"{args.id}-titlebody"
    seq_title = osc99(f"i={identifier}:p=title", "Build Complete")
    seq_body = osc99(f"i={identifier}:p=body", "All 42 tests passed in 3.7s")
    show_sequence("Title", seq_title)
    show_sequence("Body ", seq_body)
    send(seq_title)
    send(seq_body)
    wait(args)


def test_urgency(args: argparse.Namespace) -> None:
    """All three urgency levels."""
    heading("Test: Urgency Levels")
    info("Sends three notifications with low (0), normal (1), and critical (2) urgency.")
    info("Expected: platform may render critical differently (persistent, highlighted).")

    for level, label in [(0, "Low"), (1, "Normal"), (2, "Critical")]:
        identifier = f"{args.id}-urg{level}"
        seq = osc99(f"i={identifier}:u={level}", f"Urgency: {label} ({level})")
        show_sequence(f"  u={level}", seq)
        send(seq)
        time.sleep(0.3)

    wait(args)


def test_occasion(args: argparse.Namespace) -> None:
    """Display occasion filtering."""
    heading("Test: Display Occasion Filtering")
    info("Sends three notifications with different occasion filters:")
    info("  o=always    -> always shown")
    info("  o=unfocused -> only shown when terminal is NOT focused")
    info("  o=invisible -> only shown when terminal is NOT visible")
    info("")
    info("While the terminal is focused, only 'always' should appear.")
    info("Try switching focus away and re-running to see 'unfocused'.")

    for occasion in ["always", "unfocused", "invisible"]:
        identifier = f"{args.id}-occ-{occasion}"
        seq = osc99(f"i={identifier}:o={occasion}", f"Occasion: {occasion}")
        show_sequence(f"  o={occasion}", seq)
        send(seq)
        time.sleep(0.3)

    wait(args)


def test_timeout(args: argparse.Namespace) -> None:
    """Auto-close timeout."""
    heading("Test: Auto-Close Timeout")
    info("Sends a notification with w=3000 (3 second auto-close timeout).")
    info("Expected: notification disappears after ~3 seconds.")

    identifier = f"{args.id}-timeout"
    seq = osc99(f"i={identifier}:w=3000", "This closes in 3 seconds")
    show_sequence("Sequence", seq)
    send(seq)
    wait(args)


def test_app_name(args: argparse.Namespace) -> None:
    """Custom application name."""
    heading("Test: Custom Application Name")
    info("Sends a notification with f=MyApp to override the app name.")
    info("Expected: notification shows 'MyApp' as the application.")

    identifier = f"{args.id}-appname"
    seq = osc99(f"i={identifier}:f=MyApp", "Custom app name notification")
    show_sequence("Sequence", seq)
    send(seq)
    wait(args)


def test_base64(args: argparse.Namespace) -> None:
    """Base64-encoded payload."""
    heading("Test: Base64-Encoded Payload")
    info("Sends a notification with e=1 and base64-encoded title text.")

    title_text = "Decoded from base64!"
    encoded = base64.b64encode(title_text.encode()).decode()
    info(f"  Plain text: {title_text}")
    info(f"  Encoded:    {encoded}")
    info("Expected: notification shows the decoded plain text.")

    identifier = f"{args.id}-b64"
    seq = osc99(f"i={identifier}:e=1", encoded)
    show_sequence("Sequence", seq)
    send(seq)
    wait(args)


def test_chunked(args: argparse.Namespace) -> None:
    """Chunked (multi-part) payload assembly."""
    heading("Test: Chunked Payload (d=0 / d=1)")
    info("Sends a notification in three chunks using d=0, then finalizes with d=1.")
    info("Expected: single notification with combined title 'Hello Beautiful World!'")

    identifier = f"{args.id}-chunk"
    seq1 = osc99(f"i={identifier}:d=0", "Hello ")
    seq2 = osc99(f"i={identifier}:d=0", "Beautiful ")
    seq3 = osc99(f"i={identifier}:d=1", "World!")
    show_sequence("Chunk 1 (d=0)", seq1)
    show_sequence("Chunk 2 (d=0)", seq2)
    show_sequence("Chunk 3 (d=1)", seq3)
    send(seq1)
    send(seq2)
    send(seq3)
    wait(args)


def test_replace(args: argparse.Namespace) -> None:
    """Notification replacement via same identifier."""
    heading("Test: Notification Replacement")
    info("Sends a notification, then sends another with the SAME identifier.")
    info("Expected: the first notification is replaced by the second.")

    identifier = f"{args.id}-replace"
    seq1 = osc99(f"i={identifier}", "Downloading... 0%")
    show_sequence("Original", seq1)
    send(seq1)

    info("\n  Waiting 2 seconds before replacing...")
    time.sleep(2)

    seq2 = osc99(f"i={identifier}", "Download complete! 100%")
    show_sequence("Replacement", seq2)
    send(seq2)
    wait(args)


def test_close(args: argparse.Namespace) -> None:
    """Close a notification via p=close."""
    heading("Test: Close Notification")
    info("Sends a notification with a 30s timeout, then closes it after 2 seconds.")
    info("Expected: notification appears, then is dismissed programmatically.")

    identifier = f"{args.id}-close"
    seq_show = osc99(f"i={identifier}:w=30000", "This will be closed in 2s...")
    seq_close = osc99(f"i={identifier}:p=close", "")
    show_sequence("Show ", seq_show)
    show_sequence("Close", seq_close)
    send(seq_show)

    info("\n  Waiting 2 seconds before sending close...")
    time.sleep(2)

    send(seq_close)
    info("  Close sent.")
    wait(args)


def test_query(args: argparse.Namespace) -> None:
    """Capability query (p=?)."""
    heading("Test: Capability Query (p=?)")
    info("Sends a query to discover supported OSC 99 features.")
    info("Expected: terminal replies with an OSC 99 response containing capabilities.")
    info("  (The response is written to stdin; check with 'cat -v' or read the PTY.)")

    identifier = f"{args.id}-query"
    seq = osc99(f"i={identifier}:p=?", "")
    show_sequence("Sequence", seq)
    send(seq)

    info("\n  Note: Response is sent to the application's stdin as an OSC 99 sequence.")
    info("  Format: ESC]99;i=<id>:p=?;<capabilities>ESC\\")
    wait(args)


def test_alive(args: argparse.Namespace) -> None:
    """Alive query (p=alive)."""
    heading("Test: Alive Query (p=alive)")
    info("First sends a notification, then queries which notifications are still active.")
    info("Expected: terminal replies with the active notification identifier.")

    identifier = f"{args.id}-alive-notif"
    seq_show = osc99(f"i={identifier}:w=30000", "Persistent notification for alive test")
    show_sequence("Show", seq_show)
    send(seq_show)

    time.sleep(0.5)

    query_id = f"{args.id}-alive-query"
    seq_query = osc99(f"i={query_id}:p=alive", "")
    show_sequence("Alive query", seq_query)
    send(seq_query)

    info(f"\n  Response should contain '{identifier}' in the active list.")
    info("  Format: ESC]99;i=<query-id>:p=alive;<id1>,<id2>,...ESC\\")

    # Clean up
    time.sleep(0.5)
    send(osc99(f"i={identifier}:p=close", ""))

    wait(args)


def test_activation_flags(args: argparse.Namespace) -> None:
    """Activation flags: focus, report, both."""
    heading("Test: Activation Flags (a=)")
    info("Sends three notifications with different activation behaviors:")
    info("  a=focus        -> clicking focuses the terminal window")
    info("  a=report       -> clicking sends a report to the app")
    info("  a=focus,report -> clicking does both")
    info("")
    info("Click each notification to test the behavior.")

    for flags, label in [("focus", "Focus only"), ("report", "Report only"), ("focus,report", "Both")]:
        identifier = f"{args.id}-act-{flags.replace(',', '-')}"
        seq = osc99(f"i={identifier}:a={flags}:w=15000", f"Action: {label}")
        show_sequence(f"  a={flags}", seq)
        send(seq)
        time.sleep(0.3)

    wait(args)


def test_close_event(args: argparse.Namespace) -> None:
    """Close event reporting (c=1)."""
    heading("Test: Close Event Reporting (c=1)")
    info("Sends a notification requesting close event reporting.")
    info("When the notification is dismissed (by user or timeout), the terminal")
    info("should receive an OSC 99 close event on stdin.")

    identifier = f"{args.id}-closeevent"
    seq = osc99(f"i={identifier}:c=1:w=5000", "Dismiss me! (close event reported)")
    show_sequence("Sequence", seq)
    send(seq)

    info("\n  Dismiss the notification or wait 5s for auto-close.")
    info("  The terminal will receive a close event on stdin.")
    wait(args)


def test_kitchen_sink(args: argparse.Namespace) -> None:
    """All metadata flags combined in one notification."""
    heading("Test: Kitchen Sink (all flags)")
    info("Sends a single notification exercising every metadata field at once:")
    info("  i=<id>, p=title, u=2 (critical), o=always, f=KitchenSink,")
    info("  w=10000 (10s), c=1, a=focus,report")

    identifier = f"{args.id}-kitchen"
    metadata = f"i={identifier}:u=2:o=always:f=KitchenSink:w=10000:c=1:a=focus,report"
    seq_title = osc99(f"{metadata}:p=title", "All Flags Active")
    seq_body = osc99(f"i={identifier}:p=body", "Critical urgency, 10s timeout, close+activation events")
    show_sequence("Title", seq_title)
    show_sequence("Body ", seq_body)
    send(seq_title)
    send(seq_body)
    wait(args)


# -- Test registry ---------------------------------------------------------

TESTS = {
    "title": (test_title, "Simple title-only notification"),
    "title-body": (test_title_body, "Title + body (two sequences)"),
    "urgency": (test_urgency, "Urgency levels (low/normal/critical)"),
    "occasion": (test_occasion, "Display occasion filtering"),
    "timeout": (test_timeout, "Auto-close timeout (w=3000)"),
    "app-name": (test_app_name, "Custom application name (f=)"),
    "base64": (test_base64, "Base64-encoded payload (e=1)"),
    "chunked": (test_chunked, "Chunked multi-part payload (d=0/d=1)"),
    "replace": (test_replace, "Notification replacement (same id)"),
    "close": (test_close, "Close notification (p=close)"),
    "query": (test_query, "Capability query (p=?)"),
    "alive": (test_alive, "Alive query (p=alive)"),
    "activation": (test_activation_flags, "Activation flags (a=focus/report)"),
    "close-event": (test_close_event, "Close event reporting (c=1)"),
    "kitchen-sink": (test_kitchen_sink, "All flags combined"),
}


def list_tests() -> None:
    """Print available test names and descriptions."""
    print(f"\n{BOLD}Available OSC 99 tests:{RESET}\n")
    for name, (_, description) in TESTS.items():
        print(f"  {GREEN}{name:<16}{RESET} {description}")
    print()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Interactive test script for OSC 99 desktop notifications.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="list available tests and exit",
    )
    parser.add_argument(
        "--test", "-t",
        action="append",
        metavar="NAME",
        help="run specific test(s); can be repeated (default: all)",
    )
    parser.add_argument(
        "--delay", "-d",
        type=float,
        default=1.0,
        help="delay in seconds between tests in batch mode (default: 1.0)",
    )
    parser.add_argument(
        "--id",
        default="osc99",
        metavar="PREFIX",
        help="identifier prefix for notification IDs (default: osc99)",
    )
    parser.add_argument(
        "--batch", "-b",
        action="store_true",
        help="non-interactive mode: no prompts, uses --delay between tests",
    )

    args = parser.parse_args()

    if args.list:
        list_tests()
        return

    selected = args.test if args.test else list(TESTS.keys())

    # Validate names.
    for name in selected:
        if name not in TESTS:
            print(f"{RED}Unknown test: '{name}'{RESET}", file=sys.stderr)
            print(f"Use --list to see available tests.", file=sys.stderr)
            sys.exit(1)

    heading("OSC 99 Desktop Notification Test Suite")
    info(f"Running {len(selected)} test(s): {', '.join(selected)}")
    info(f"Identifier prefix: {args.id}")
    info(f"Mode: {'batch (delay={:.1f}s)'.format(args.delay) if args.batch else 'interactive'}")

    for name in selected:
        func, _ = TESTS[name]
        func(args)

    print(f"\n{BOLD}{GREEN}All tests completed.{RESET}\n")


if __name__ == "__main__":
    main()
