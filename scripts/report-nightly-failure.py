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
import subprocess
import sys

LABEL = "ci-nightly"
# What a report may quote from the failed jobs' logs; an issue body takes 65536 characters at most.
LOG_EXCERPT_CHARACTERS = 20000


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
        f"<details><summary>End of the failed jobs' logs</summary>\n\n```\n{excerpt}\n```\n\n</details>\n"
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
    for failure in failures:
        print(f"report-nightly-failure self-test: FAILED: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"report-nightly-failure self-test: {len(cases) + 4} cases OK.")
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
    failed_jobs = [job["name"] for job in run["jobs"] if job.get("conclusion") == "failure"]
    log = _gh(["run", "view", arguments.run_id, "--repo", repo, "--log-failed"], check=False).stdout
    body = failure_text(arguments.workflow, run_url, failed_jobs, log)
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
