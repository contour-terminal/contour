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

import sys

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


def main() -> int:
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
