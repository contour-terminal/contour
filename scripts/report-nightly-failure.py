#! /usr/bin/env python3
"""Keeps one GitHub issue per nightly workflow that says whether its last run failed.

A nightly run has no pull request to turn red, so nobody sees it fail. After each scheduled run this
opens an issue when the run failed and none is open, adds a comment when one already is, and closes
it when the run passed again -- so the notification is the issue, and an open issue always means
"still failing". With a token that may assign issues to the Copilot coding agent, a new issue is
handed to it.

Run by the `report` job of a nightly workflow; see docs/internals/build-times.md ("Nightly runs").
"""

from __future__ import annotations

import argparse
import enum
import json
import os
import re
import subprocess
import sys

LABEL = "ci-nightly"
# What a report may quote from the failed jobs' logs; an issue body takes 65536 characters at most.
LOG_EXCERPT_CHARACTERS = 20000
# What is quoted of one log line: a compiler's command line runs to thousands of characters and says little.
LOG_LINE_CHARACTERS = 300


class Action(enum.Enum):
    """What one nightly run does to its workflow's issue."""

    NONE = "none"
    OPEN = "open"
    COMMENT = "comment"
    CLOSE = "close"


def action_for(run_failed: bool, issue_is_open: bool) -> Action:
    """Returns what a run's outcome does to the workflow's issue."""
    # Data, so that a third outcome (cancelled, say) is one more row.
    table = {
        (True, False): Action.OPEN,
        (True, True): Action.COMMENT,
        (False, True): Action.CLOSE,
        (False, False): Action.NONE,
    }
    return table[(run_failed, issue_is_open)]


def issue_title(workflow: str) -> str:
    """Returns the title that identifies a workflow's issue; it is how the open one is found again."""
    return f"Nightly {workflow} is failing"


# What the runner puts in front of every log line, and the colour codes a compiler adds.
_LOG_LINE_PREFIX = re.compile(r"^\d{4}-\d{2}-\d{2}T[\d:.]+Z ")
_COLOUR_CODE = re.compile(r"\x1b\[[0-9;]*m")
# What marks a line as the reason a job failed, across compilers, clang-tidy, ninja, CTest and the runner.
_DIAGNOSTIC = re.compile(r"\berror\b[: ]|\bFAILED\b|\*\*\*Failed|\*\*\*Timeout|##\[error\]|\bfatal\b")
# How many lines after a diagnostic belong to it (the source line, the caret, a note).
_LINES_AFTER_DIAGNOSTIC = 4
# What is quoted of a log that names no diagnostic at all.
_TAIL_LINES = 150


def diagnostic_excerpt(log: str) -> str:
    """Returns the lines of a job's log that say why it failed, or its end if none does."""
    lines = [_COLOUR_CODE.sub("", _LOG_LINE_PREFIX.sub("", line)) for line in log.splitlines()]
    kept: list[str] = []
    keep_until = -1
    for index, line in enumerate(lines):
        if _DIAGNOSTIC.search(line):
            if kept and index > keep_until + 1:
                kept.append("[...]")
            keep_until = index + _LINES_AFTER_DIAGNOSTIC
        if index <= keep_until:
            kept.append(line if len(line) <= LOG_LINE_CHARACTERS else line[:LOG_LINE_CHARACTERS] + " [...]")
    return "\n".join(kept if kept else lines[-_TAIL_LINES:])


def failed_job_logs(repo: str, jobs: list[dict[str, object]]) -> str:
    """Returns the excerpt of each failed job's log, through the `gh` CLI.

    Per job, not `gh run view --log-failed`: that prints nothing while the run it is asked about is still
    in progress, which it is for the job that reports it.
    """
    excerpts: list[str] = []
    for job in jobs:
        endpoint = f"repos/{repo}/actions/jobs/{job['databaseId']}/logs"
        # A log carries colour codes, which a newer gh prints only when told to; an older one has no such flag.
        fetched = _gh(["api", "--allow-escape-sequences", endpoint], check=False)
        if fetched.returncode != 0:
            fetched = _gh(["api", endpoint], check=False)
        excerpt = diagnostic_excerpt(fetched.stdout) or f"(no log: {fetched.stderr.strip()})"
        excerpts.append(f"== {job['name']}\n{excerpt}")
    return "\n\n".join(excerpts)


def failure_text(workflow: str, run_url: str, failed_jobs: list[str], log_excerpt: str) -> str:
    """Returns the body of a new issue, or of the comment a further failed night adds."""
    jobs = "\n".join(f"- {job}" for job in failed_jobs) or "- (no job reported a failure; see the run)"
    excerpt = log_excerpt[-LOG_EXCERPT_CHARACTERS:]
    return (
        f"The scheduled run of the **{workflow}** workflow failed: {run_url}\n\n"
        f"Failed jobs:\n{jobs}\n\n"
        "What to do: find the cause in the log below, fix it at its source (the repository's rules are in "
        "`AGENT.md`: no suppressed warnings, no `NOLINT`), and open a pull request. This issue is closed by the "
        "next scheduled run that passes.\n\n"
        f"<details><summary>What the failed jobs' logs say</summary>\n\n```\n{excerpt}\n```\n\n</details>\n"
    )


def _gh(arguments: list[str], token: str | None = None, check: bool = True) -> subprocess.CompletedProcess[str]:
    environment = dict(os.environ)
    if token:
        environment["GH_TOKEN"] = token
    return subprocess.run(["gh", *arguments], check=check, capture_output=True, text=True, env=environment)


def open_issue_number(repo: str, title: str) -> int | None:
    """Returns the number of the open issue with exactly this title and the nightly label, if any."""
    listing = _gh(["issue", "list", "--repo", repo, "--state", "open", "--label", LABEL, "--json", "number,title"])
    return next((issue["number"] for issue in json.loads(listing.stdout) if issue["title"] == title), None)


def self_test() -> int:
    cases = (
        ("a first failed night opens the issue", True, False, Action.OPEN),
        ("a further failed night comments on it", True, True, Action.COMMENT),
        ("a passing night closes it", False, True, Action.CLOSE),
        ("a passing night with no issue does nothing", False, False, Action.NONE),
    )
    failures = [
        f"{name}: expected {expected}, got {action_for(failed, is_open)}"
        for name, failed, is_open, expected in cases
        if action_for(failed, is_open) is not expected
    ]
    body = failure_text("Clang-tidy", "https://example.invalid/run/1", ["Check clang-tidy"], "x" * 50000 + "LAST")
    if "- Check clang-tidy" not in body or "https://example.invalid/run/1" not in body:
        failures.append("the body names the run and the failed jobs")
    if not body.rstrip().endswith("</details>") or "LAST" not in body or len(body) > LOG_EXCERPT_CHARACTERS + 2000:
        failures.append("the body keeps the END of the log, within the excerpt limit")
    if "(no job reported a failure" not in failure_text("W", "u", [], ""):
        failures.append("a run without a failed job still says so")
    if issue_title("Clang-tidy") != "Nightly Clang-tidy is failing":
        failures.append("the title is what finds the issue again")
    stamp = "2026-11-13T09:19:58.1234567Z "
    noise = [f"{stamp}[{step}/900] Building CXX object a{step}.o" for step in range(300)]
    finding = f"{stamp}\x1b[1msrc/a.cpp:3:7: \x1b[0m\x1b[0;1;31merror: \x1b[0mpass by value [modernize-pass-by-value]"
    log = "\n".join([*noise[:150], finding, f"{stamp}  3 | A(B b);", *noise[150:], f"{stamp}##[error]Process completed with exit code 1."])
    excerpt = diagnostic_excerpt(log)
    if "src/a.cpp:3:7: error: pass by value [modernize-pass-by-value]" not in excerpt:
        failures.append("a diagnostic in the middle of the log is quoted, without timestamp or colour codes")
    if "  3 | A(B b);" not in excerpt or "a10.o" in excerpt:
        failures.append("a diagnostic keeps the lines after it; build progress far from one is left out")
    quiet = "\n".join(f"{stamp}line {number}" for number in range(400))
    if "line 399" not in diagnostic_excerpt(quiet) or "line 0\n" in diagnostic_excerpt(quiet):
        failures.append("a log without a diagnostic is quoted from its end")
    command = f"{stamp}FAILED: a.o\n{stamp}/usr/bin/clang++ " + "-Dx " * 4000
    if max(len(line) for line in diagnostic_excerpt(command).splitlines()) > LOG_LINE_CHARACTERS + 10:
        failures.append("a compiler command line is cut short")
    if diagnostic_excerpt("") != "":
        failures.append("an empty log is an empty excerpt")
    for failure in failures:
        print(f"report-nightly-failure self-test: FAILED: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"report-nightly-failure self-test: {len(cases) + 9} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the rules against inline fixtures")
    parser.add_argument("--repo", help="the repository, as OWNER/NAME")
    parser.add_argument("--workflow", help="the workflow's display name")
    parser.add_argument("--run-id", help="the run to report")
    parser.add_argument("--outcome", choices=("success", "failure"), help="how the run's jobs ended")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    if not (arguments.repo and arguments.workflow and arguments.run_id and arguments.outcome):
        parser.error("--repo, --workflow, --run-id and --outcome are required")

    repo = arguments.repo
    title = issue_title(arguments.workflow)
    number = open_issue_number(repo, title)
    action = action_for(arguments.outcome == "failure", number is not None)
    print(f"report-nightly-failure: {arguments.workflow} ended in {arguments.outcome}; action: {action.value}")
    if action is Action.NONE:
        return 0

    run_url = f"https://github.com/{repo}/actions/runs/{arguments.run_id}"
    if action is Action.CLOSE:
        _gh(["issue", "close", str(number), "--repo", repo, "--comment", f"The scheduled run passed again: {run_url}"])
        return 0

    run = json.loads(_gh(["run", "view", arguments.run_id, "--repo", repo, "--json", "jobs"]).stdout)
    failed_jobs = [job for job in run["jobs"] if job.get("conclusion") == "failure"]
    log = failed_job_logs(repo, failed_jobs)
    body = failure_text(arguments.workflow, run_url, [job["name"] for job in failed_jobs], log)
    if action is Action.COMMENT:
        _gh(["issue", "comment", str(number), "--repo", repo, "--body", body])
        return 0

    _gh(["label", "create", LABEL, "--repo", repo, "--force", "--description", "A scheduled CI run is failing"])
    created = _gh(["issue", "create", "--repo", repo, "--title", title, "--label", LABEL, "--body", body])
    issue_url = created.stdout.strip().splitlines()[-1]
    print(f"report-nightly-failure: opened {issue_url}")
    # Handing the issue to the Copilot coding agent needs a user's token: the workflow's own cannot assign it.
    assign_token = os.environ.get("NIGHTLY_ASSIGN_TOKEN")
    if assign_token:
        assigned = _gh(["issue", "edit", issue_url, "--add-assignee", "@copilot"], token=assign_token, check=False)
        outcome = "assigned to Copilot" if assigned.returncode == 0 else f"not assigned: {assigned.stderr.strip()}"
        print(f"report-nightly-failure: {outcome}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
