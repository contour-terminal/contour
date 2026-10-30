#! /usr/bin/env python3
"""Validates the GitHub Actions workflow files with actionlint.

This is the same schema check GitHub runs when it compiles a workflow. It catches locally the class
of error that silently breaks CI dispatch -- e.g. an invalid `${{ env.X }}` context in a job-level
`env:` block makes GitHub create a check-suite with zero jobs and fail at 0s, which is invisible
without this check.

actionlint is fetched on demand into the build tree if not already on PATH, so this needs no system
install. If it cannot be obtained (offline), the check SKIPS rather than fails, so a fresh offline
clone still gets a green `ctest`. `.github/workflows/build.yml` sets CHECK_WORKFLOWS_REQUIRE_TOOL=1
at workflow level to turn that skip into a hard failure, so a failed download can never show up as
a green gate in CI.

It is Python rather than a shell script so that it runs on Windows too, alongside the spell gate.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import Any

from lib.toolcache import REPO_ROOT, ToolSpec, resolve, run

# The pinned version. The bash script this replaces ran actionlint's own installer, which always
# fetched the newest release -- so two runs a month apart could lint against different rules. That
# installer is itself a bash script and cannot run on Windows, so the port has to name a version
# anyway; pinning it brings actionlint in line with typos. Bump this line to upgrade.
ACTIONLINT_VERSION = "1.7.12"

_RELEASE = f"https://github.com/rhysd/actionlint/releases/download/v{ACTIONLINT_VERSION}"

ACTIONLINT = ToolSpec(
    name="actionlint",
    version=ACTIONLINT_VERSION,
    url_template=f"{_RELEASE}/{{asset}}",
    assets={
        ("Linux", "x86_64"): f"actionlint_{ACTIONLINT_VERSION}_linux_amd64.tar.gz",
        ("Linux", "aarch64"): f"actionlint_{ACTIONLINT_VERSION}_linux_arm64.tar.gz",
        ("Darwin", "x86_64"): f"actionlint_{ACTIONLINT_VERSION}_darwin_amd64.tar.gz",
        ("Darwin", "aarch64"): f"actionlint_{ACTIONLINT_VERSION}_darwin_arm64.tar.gz",
        ("Windows", "x86_64"): f"actionlint_{ACTIONLINT_VERSION}_windows_amd64.zip",
        ("Windows", "aarch64"): f"actionlint_{ACTIONLINT_VERSION}_windows_arm64.zip",
        ("FreeBSD", "x86_64"): f"actionlint_{ACTIONLINT_VERSION}_freebsd_amd64.tar.gz",
    },
    require_env="CHECK_WORKFLOWS_REQUIRE_TOOL",
    cache_env="ACTIONLINT_CACHE_DIR",
)

WORKFLOW_DIR = REPO_ROOT / ".github" / "workflows"

# Only fail on ERRORS that break workflow compilation, not on style warnings about pinned action
# versions or shellcheck nits (which the repo carries pre-existing and which do not stop dispatch).
# actionlint groups those under specific rule ids; -ignore filters them by message pattern.
IGNORED_MESSAGES = (
    r'the runner of ".*" action is too old',
    r'property ".*" is not defined in object type',
    r"shellcheck reported issue",
    r"constant expression .* in condition",
    r'label "ubuntu-26\.04.*" is unknown',
)

# The status checks branch protection waits for, by the display name GitHub reports (the job's
# `name:`, else its id). A pull request that cannot produce one of these is blocked for good, so
# the rule below keeps them runnable on pull requests however much else is gated to master.
REQUIRED_CONTEXTS = frozenset(
    {
        "Windows",
        "tests matrix",
        "Check C++ style",
        "Check editorconfig",
        "Ubuntu Linux 24.04 post-check",
        "macOSArm",
    }
)

# An `if:` containing one of these is false for an ordinary pull request. Data, so that a new way of
# saying "master only" is one more row.
PR_SKIP_PATTERNS = (
    re.compile(r"github\.event_name\s*!=\s*['\"]pull_request['\"]"),
    re.compile(r"github\.ref\s*==\s*['\"]refs/heads/master['\"]"),
    re.compile(r"^\s*(\$\{\{\s*)?false\s*(\}\})?\s*$"),
)

# What lets a job run although a job it needs was skipped.
_RUNS_DESPITE_SKIPPED_NEEDS = re.compile(r"\b(always|!cancelled)\(\)")
_NEEDS_RESULT_NOT_SUCCESS = re.compile(r"needs\.([\w-]+)\.result\s*!=\s*['\"]success['\"]")


def _condition(node: dict[str, Any]) -> str:
    return str(node.get("if", ""))


def _skipped_on_pull_requests(condition: str) -> bool:
    return any(pattern.search(condition) for pattern in PR_SKIP_PATTERNS)


def _needs(job: dict[str, Any]) -> list[str]:
    needs = job.get("needs", [])
    return [needs] if isinstance(needs, str) else list(needs)


def _pr_skipped_jobs(jobs: dict[str, Any]) -> set[str]:
    """Ids of the jobs that do not run on an ordinary pull request, including those that only
    inherit the skip by needing one without `always()`."""
    skipped = {job_id for job_id, job in jobs.items() if _skipped_on_pull_requests(_condition(job))}
    changed = True
    while changed:
        changed = False
        for job_id, job in jobs.items():
            inherits = any(need in skipped for need in _needs(job)) and not _RUNS_DESPITE_SKIPPED_NEEDS.search(
                _condition(job)
            )
            if inherits and job_id not in skipped:
                skipped.add(job_id)
                changed = True
    return skipped


def check_required_contexts(jobs: dict[str, Any]) -> list[str]:
    """Returns the findings for the jobs of one workflow whose name is a required status check."""
    findings: list[str] = []
    skipped = _pr_skipped_jobs(jobs)
    for job_id, job in jobs.items():
        name = str(job.get("name", job_id))
        if name not in REQUIRED_CONTEXTS:
            continue
        condition = _condition(job)
        if _skipped_on_pull_requests(condition):
            findings.append(f"{job_id} ({name}): required check, but `if: {condition}` skips it on pull requests")
            continue
        skipped_needs = [need for need in _needs(job) if need in skipped]
        if not skipped_needs:
            continue
        if not _RUNS_DESPITE_SKIPPED_NEEDS.search(condition):
            findings.append(
                f"{job_id} ({name}): needs {skipped_needs}, skipped on pull requests, "
                "so it needs `if: always()` to report there at all"
            )
        for step in job.get("steps", []):
            step_condition = _condition(step)
            for need in _NEEDS_RESULT_NOT_SUCCESS.findall(step_condition):
                if need in skipped and "'skipped'" not in step_condition:
                    findings.append(
                        f"{job_id} ({name}): step `{step.get('name', '?')}` fails when {need} is not "
                        f"'success', but {need} is skipped on pull requests; accept 'skipped' too"
                    )
    return findings


def _load_yaml() -> Any:
    try:
        import yaml  # PyYAML; optional, see check_workflow_files

        return yaml
    except ImportError:
        return None


# Set (to anything) where PyYAML is guaranteed, to turn "PyYAML missing" from a skip into a failure.
# It is separate from CHECK_WORKFLOWS_REQUIRE_TOOL, which build.yml sets for every job: PyYAML is not
# on every host (the Windows and macOS runners), so tying the two together would break those jobs.
# The Linux matrix installs python3-yaml and sets this, so a pull request always runs the rule.
REQUIRE_YAML_ENV = "CHECK_WORKFLOWS_REQUIRE_YAML"


def _without_yaml(require: bool) -> int:
    """Outcome of the required-contexts rule when PyYAML is not importable."""
    if require:
        print(f"check-workflows: PyYAML is required ({REQUIRE_YAML_ENV} is set) but not installed.", file=sys.stderr)
        return 1
    # actionlint is the gate that must not be bypassed; this rule is a second layer, so by default
    # it skips rather than failing.
    print("required-contexts rule: PyYAML not installed; skipped.")
    return 0


def check_workflow_files(workflows: list[Any]) -> int:
    yaml = _load_yaml()
    if yaml is None:
        return _without_yaml(require=bool(os.environ.get(REQUIRE_YAML_ENV)))
    findings: list[str] = []
    for path in workflows:
        document = yaml.safe_load(path.read_text(encoding="utf-8"))
        for finding in check_required_contexts(document.get("jobs", {})):
            findings.append(f"{path.name}: {finding}")
    for finding in findings:
        print(f"check-workflows: {finding}", file=sys.stderr)
    return 1 if findings else 0


# Fixtures are the parsed form of a workflow's `jobs:` mapping, spelled as Python so that the
# self-test needs no YAML parser (PyYAML is optional here, see check_workflow_files).
_SKIPPED_ON_PRS = {"if": "github.event_name != 'pull_request'", "steps": []}
_FAIL_UNLESS_SKIPPED_OR_OK = "${{ needs.%s.result != 'success' && needs.%s.result != 'skipped' }}"

_FIXTURE_BROKEN = {
    "test_notcurses": _SKIPPED_ON_PRS,
    "check_notcurses_test_matrix": {
        "name": "tests matrix",
        "needs": "test_notcurses",
        "steps": [{"if": "${{ needs.test_notcurses.result != 'success' }}", "run": "exit 1"}],
    },
}

_FIXTURE_FIXED = {
    "test_notcurses": _SKIPPED_ON_PRS,
    "check_notcurses_test_matrix": {
        "if": "${{ always() }}",
        "name": "tests matrix",
        "needs": "test_notcurses",
        "steps": [{"if": _FAIL_UNLESS_SKIPPED_OR_OK % ("test_notcurses", "test_notcurses"), "run": "exit 1"}],
    },
}

_FIXTURE_GATED_REQUIRED = {"windows": {"name": "Windows", **_SKIPPED_ON_PRS}}

_FIXTURE_NO_ALWAYS = {
    "package": _SKIPPED_ON_PRS,
    "gate": {
        "name": "Ubuntu Linux 24.04 post-check",
        "needs": ["package"],
        "steps": [{"if": _FAIL_UNLESS_SKIPPED_OR_OK % ("package", "package"), "run": "exit 1"}],
    },
}

# The carve-out build.yml uses for the `release` branch's pull request still skips ordinary ones.
_FIXTURE_RELEASE_CARVE_OUT = {
    "windows": {
        "name": "Windows",
        "if": "github.event_name != 'pull_request' || github.head_ref == 'release'",
        "steps": [],
    },
}

_FIXTURE_UNRELATED = {
    "heavy": _SKIPPED_ON_PRS,
    "lint": {"name": "not required", "needs": "heavy", "steps": []},
}


def self_test() -> int:
    cases = (
        ("success-only gate over a PR-skipped need fires", _FIXTURE_BROKEN, 2),
        ("always() plus accepting 'skipped' passes", _FIXTURE_FIXED, 0),
        ("a required job gated off pull requests fires", _FIXTURE_GATED_REQUIRED, 1),
        ("a required job needing a skipped job without always() fires", _FIXTURE_NO_ALWAYS, 1),
        ("a job that is not a required context may be gated", _FIXTURE_UNRELATED, 0),
        ("the head_ref == 'release' carve-out still skips pull requests", _FIXTURE_RELEASE_CARVE_OUT, 1),
    )
    failures = []
    for description, fixture, expected in cases:
        found = check_required_contexts(fixture)
        if len(found) != expected:
            failures.append(f"{description}: expected {expected} finding(s), got {found}")
    if _without_yaml(require=True) != 1:
        failures.append("a missing PyYAML must fail when it is required")
    if _without_yaml(require=False) != 0:
        failures.append("a missing PyYAML must skip when it is not required")
    for failure in failures:
        print(f"check-workflows self-test: FAILED: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"check-workflows self-test: {len(cases) + 2} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="check the rules against inline fixtures")
    if parser.parse_args().self_test:
        return self_test()

    # Only workflow files that actually exist; nothing to do otherwise.
    if not WORKFLOW_DIR.is_dir():
        print("No .github/workflows directory; nothing to lint.")
        return 0

    # Globbed here rather than by the shell: neither cmd nor PowerShell expands wildcards for the
    # program it launches, so relying on that would have made this a Linux-only check again.
    workflows = sorted(WORKFLOW_DIR.glob("*.yml"))
    if not workflows:
        print("No workflow files; nothing to lint.")
        return 0

    actionlint = resolve(ACTIONLINT)

    arguments: list[str] = []
    for message in IGNORED_MESSAGES:
        arguments += ["-ignore", message]
    arguments += [str(path.relative_to(REPO_ROOT)) for path in workflows]

    exit_code = run(actionlint, arguments)
    if exit_code != 0:
        return exit_code

    print(f"actionlint: workflows OK ({len(workflows)} files).")
    return check_workflow_files(workflows)


if __name__ == "__main__":
    sys.exit(main())
