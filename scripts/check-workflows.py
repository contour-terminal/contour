#! /usr/bin/env python3
"""Validates the GitHub Actions workflow files with actionlint and a few rules of our own.

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
import json
import os
import re
import sys
from collections.abc import Iterator
from typing import Any

from lib.compiler_caches import saves_compiler_cache
from lib.toolcache import REPO_ROOT, ToolSpec, download_self_test, resolve, run

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
        # PR-only by design (its full-tree sibling, "Check clang-tidy", lives in clang-tidy.yml, which no
        # pull request triggers).
        "Check clang-tidy (diff)",
        # Lives in changelog.yml; every workflow file is checked, so it is covered too.
        "Check Changelog Action",
        "Ubuntu Linux 24.04 post-check",
        "macOSArm",
    }
)

# The one spelling of "this run is for a release tag" (tags are v<major>.<minor>.<patch>; the trigger
# filter narrows them further). Every use is spelled exactly like this so it stays greppable.
RELEASE_TAG_CONDITION = "startsWith(github.ref, 'refs/tags/v')"

# An `if:` containing one of these is false for an ordinary pull request. Data, so that a new way of
# saying "master only" is one more row.
PR_SKIP_PATTERNS = (
    re.compile(r"github\.event_name\s*!=\s*['\"]pull_request['\"]"),
    re.compile(r"github\.ref\s*==\s*['\"]refs/heads/master['\"]"),
    re.compile(r"^\s*(\$\{\{\s*)?false\s*(\}\})?\s*$"),
    # A release tag is never a pull request's ref.
    re.compile(re.escape(RELEASE_TAG_CONDITION)),
)

# Text that must not appear anywhere in a workflow file, comments included, with the reason. Data, so
# that retiring another condition is one more row.
RETIRED_PATTERNS: tuple[tuple[re.Pattern[str], str], ...] = (
    (
        re.compile(r"head_ref\s*[!=]=\s*['\"]release['\"]"),
        "the `release` branch's pull request no longer builds releases; a release tag does "
        "(see docs/internals/releasing.md)",
    ),
)

# Steps that create or change a release. Only a job gated on RELEASE_TAG_CONDITION may contain one.
RELEASE_WRITERS: tuple[re.Pattern[str], ...] = (
    re.compile(r"\bgh\s+release\s+(create|upload|edit|delete)\b"),
    re.compile(r"softprops/action-gh-release"),
    re.compile(r"\bgh\s+api\b[^\n]*releases"),
)

# The script that deletes the generations the compiler-cache savers supersede. Which steps save one is
# the family table of scripts/lib/compiler_caches.py.
PRUNE_SCRIPT = "prune-cache-generations.py"

PRESET_DIR = REPO_ROOT / "cmake" / "presets"

# The CMake option that selects the production configuration, and the values CMake reads as true (compared
# case-insensitively; CMake also takes Y and a non-zero number, which nobody here writes).
PRODUCTION_OPTION = "CONTOUR_PRODUCTION_BUILD"
CMAKE_TRUE = ("ON", "TRUE", "YES", "1")

# Text in a job that means it builds the production configuration without naming a production preset:
# the option itself, set true (`-D X=ON`, `-DX=ON`, `-DX:BOOL=true`, ...), and the scripts that set it. Data,
# so that another such script is one more row.
PRODUCTION_MARKERS: tuple[re.Pattern[str], ...] = (
    re.compile(rf"(?:\b|-D){PRODUCTION_OPTION}(?::BOOL)?=(?:{'|'.join(CMAKE_TRUE)})\b", re.IGNORECASE),
    # Configures with -DCONTOUR_PRODUCTION_BUILD=ON itself, out of this rule's sight but for this row.
    re.compile(r"\bscripts/build-appimage\.sh\b"),
)

# A `--preset` argument of cmake, ctest or cpack, and the preset it names.
_PRESET_ARGUMENT = re.compile(r"--preset[\s=]+[\"']?(?P<name>[\w.-]+)")

# The jobs that build the production configuration on pull requests anyway, each with the reason. Every
# other such job must not run on a pull request: its link-time optimization costs a pull request the
# better part of an hour per platform, and the plain configuration tests the same code
# (docs/internals/build-times.md, "Why pull requests build the plain configuration").
PRODUCTION_ON_PULL_REQUESTS: dict[str, str] = {
    # The only job that builds the GUI-less configuration, so it cannot wait for master; about 10 minutes.
    "package_daemon_for_Ubuntu": "the only GUI-less build",
    # Short, and Linux. Its production flag is set inside scripts/build-appimage.sh, not in the workflow,
    # which is why that script is a row of PRODUCTION_MARKERS.
    "package_for_AppImage": "the AppImage build, a short Linux job",
}

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


def check_text(text: str) -> list[str]:
    """Returns the findings that need only a workflow file's text: retired conditions anywhere
    (comments included), and `refs/tags/` spelled other than as RELEASE_TAG_CONDITION."""
    findings = [
        f"`{match.group(0)}` is retired: {reason}"
        for pattern, reason in RETIRED_PATTERNS
        for match in pattern.finditer(text)
    ]
    if text.count("refs/tags/") != text.count(RELEASE_TAG_CONDITION):
        findings.append(
            f"refs/tags/ appears outside the release-tag condition {RELEASE_TAG_CONDITION}; "
            "spell it exactly so it stays greppable"
        )
    return findings


def _gated_on_release_tag(condition: str) -> bool:
    """Whether `condition` is false unless the run is for a release tag: no `||`, and one of its `&&`
    conjuncts is RELEASE_TAG_CONDITION itself (so `!startsWith(...)` does not count)."""
    expression = condition.strip().removeprefix("${{").removesuffix("}}").strip()
    if "||" in expression:
        return False
    return RELEASE_TAG_CONDITION in (conjunct.strip() for conjunct in expression.split("&&"))


def check_release_writers(jobs: dict[str, Any]) -> list[str]:
    """Returns the findings for the jobs of one workflow that write releases without being gated on
    RELEASE_TAG_CONDITION. A job-level `uses:` (a reusable workflow) counts as a writer too."""
    findings: list[str] = []
    for job_id, job in jobs.items():
        if _gated_on_release_tag(_condition(job)):
            continue
        writers = [(f"job `{job_id}`", str(job.get("uses", "")))] + [
            (f"step `{step.get('name', '?')}`", f"{step.get('run', '')}\n{step.get('uses', '')}")
            for step in job.get("steps", [])
        ]
        findings += [
            f"{job_id}: {what} writes a release, but the job's `if:` is not gated on {RELEASE_TAG_CONDITION} alone"
            for what, text in writers
            if any(pattern.search(text) for pattern in RELEASE_WRITERS)
        ]
    return findings


def check_master_only_jobs_queue(jobs: dict[str, Any]) -> list[str]:
    """Returns the findings for the jobs of one workflow that no pull request runs and that do not queue
    behind their own previous run: each needs a job-level concurrency group of its own per ref (and per
    matrix leg), without cancel-in-progress. Jobs gated on a release tag alone are exempt."""
    findings: list[str] = []
    for job_id, job in jobs.items():
        condition = _condition(job)
        if not _skipped_on_pull_requests(condition) or _gated_on_release_tag(condition):
            continue
        concurrency = job.get("concurrency")
        settings = concurrency if isinstance(concurrency, dict) else {"group": concurrency or ""}
        group = str(settings.get("group", ""))
        # What the group must name, with what goes wrong without it. Data, so that a further
        # requirement is one more row.
        requirements = (
            ("github.ref", True, "or a release tag's job would queue behind master's"),
            (job_id, True, "or it would queue behind other jobs"),
            ("strategy.job-index", "matrix" in job.get("strategy", {}), "or its matrix legs would queue behind each other"),
        )
        missing = [f"`{text}` ({why})" for text, required, why in requirements if required and text not in group]
        if missing:
            findings.append(
                f"{job_id}: no pull request runs it, so its `concurrency.group` must name {', '.join(missing)}; "
                "without a group of its own it overlaps with the same job of the previous push"
            )
        elif str(settings.get("cancel-in-progress", False)).lower() != "false":
            findings.append(f"{job_id}: `cancel-in-progress` must be off, or a push would cancel the running job")
    return findings


def check_llvm_versions(versions: dict[str, str]) -> list[str]:
    """Returns a finding when the workflow files that set a workflow-level `LLVM_VERSION` disagree.
    `versions` maps each such file's name to its value; a file that does not set it is absent."""
    if len(set(versions.values())) <= 1:
        return []
    listed = ", ".join(f"{name} has {value!r}" for name, value in sorted(versions.items()))
    return [f"LLVM_VERSION differs between workflow files ({listed}); it is one toolchain, so set one value"]


def check_prune_covers_savers(jobs: dict[str, Any]) -> list[str]:
    """Returns the findings for the jobs of one workflow that save a compiler cache without the job that
    prunes the superseded generations waiting for them (see scripts/prune-cache-generations.py)."""

    savers = sorted(
        job_id for job_id, job in jobs.items() if any(saves_compiler_cache(step) for step in job.get("steps", []))
    )
    if not savers:
        return []
    pruners = [job for job in jobs.values() if any(PRUNE_SCRIPT in str(step.get("run", "")) for step in job.get("steps", []))]
    if not pruners:
        return [f"{', '.join(savers)}: save a compiler cache, but no job runs {PRUNE_SCRIPT}"]
    awaited = {need for job in pruners for need in _needs(job)}
    return [
        f"{job_id}: saves a compiler cache, but the job running {PRUNE_SCRIPT} does not `needs:` it, so its "
        "superseded generation would stay until the next build"
        for job_id in savers
        if job_id not in awaited
    ]


def production_presets(documents: Any) -> frozenset[str]:
    """Returns the names of the presets that build the production configuration: the visible configure
    presets whose PRODUCTION_OPTION resolves to true -- their own `cacheVariables`, else the first parent in
    `inherits` that sets it, as CMake resolves it -- and the build, test and package presets made from one
    of those. `documents` are the parsed preset files."""
    documents = list(documents)
    configure_presets = {
        preset["name"]: preset for document in documents for preset in document.get("configurePresets", [])
    }

    def option(name: str, seen: frozenset[str] = frozenset()) -> str | None:
        preset = configure_presets.get(name, {})
        value = preset.get("cacheVariables", {}).get(PRODUCTION_OPTION)
        if value is not None:
            return str(value.get("value", "") if isinstance(value, dict) else value)
        parents = preset.get("inherits", [])
        for parent in [parents] if isinstance(parents, str) else parents:
            inherited = option(parent, seen | {name}) if parent not in seen else None
            if inherited is not None:
                return inherited
        return None

    configure = {
        name
        for name, preset in configure_presets.items()
        if not preset.get("hidden") and (option(name) or "").upper() in CMAKE_TRUE
    }
    derived = (
        preset
        for document in documents
        for kind in ("buildPresets", "testPresets", "packagePresets")
        for preset in document.get(kind, [])
    )
    return frozenset(configure | {preset["name"] for preset in derived if preset.get("configurePreset") in configure})


def _scalars(node: Any) -> Iterator[str]:
    """Every scalar value of a parsed YAML node, as text: `run`, `env` and `with` values alike."""
    if isinstance(node, dict):
        for value in node.values():
            yield from _scalars(value)
    elif isinstance(node, list):
        for value in node:
            yield from _scalars(value)
    elif node is not None:
        yield str(node)


def _builds_production(job: dict[str, Any], presets: frozenset[str]) -> bool:
    """Whether a job builds the production configuration: a `--preset` naming one of `presets`, a value
    that is one of them (an `env` variable a step expands, an action's input), or a PRODUCTION_MARKERS row."""
    for text in _scalars(job):
        if text.strip() in presets or any(marker.search(text) for marker in PRODUCTION_MARKERS):
            return True
        if any(match["name"] in presets for match in _PRESET_ARGUMENT.finditer(text)):
            return True
    return False


def check_production_off_pull_requests(jobs: dict[str, Any], presets: frozenset[str]) -> list[str]:
    """Returns the findings for the jobs of one workflow that build the production configuration on pull
    requests without a row in PRODUCTION_ON_PULL_REQUESTS, and for the rows of that table that name a job of
    this workflow which no longer needs one. `presets` are the production presets (production_presets())."""
    skipped = _pr_skipped_jobs(jobs)
    findings: list[str] = []
    for job_id, job in jobs.items():
        on_pull_requests = job_id not in skipped
        production = _builds_production(job, presets)
        excepted = job_id in PRODUCTION_ON_PULL_REQUESTS
        if production and on_pull_requests and not excepted:
            findings.append(
                f"{job_id}: builds the production configuration on pull requests; gate it with "
                "`if: github.event_name != 'pull_request'` and give pull requests a plain-preset job, "
                "or add it to PRODUCTION_ON_PULL_REQUESTS with the reason"
            )
        elif excepted and not (production and on_pull_requests):
            findings.append(
                f"{job_id}: listed in PRODUCTION_ON_PULL_REQUESTS ({PRODUCTION_ON_PULL_REQUESTS[job_id]}), but it "
                "no longer builds the production configuration on pull requests; remove the row"
            )
    return findings


def _load_production_presets() -> frozenset[str]:
    return production_presets(
        json.loads(path.read_text(encoding="utf-8")) for path in sorted(PRESET_DIR.glob("*.json"))
    )


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
    print(
        "YAML rules (required contexts, release writers, cache prune, master-only queues, production builds, "
        "LLVM_VERSION): PyYAML not installed; skipped."
    )
    return 0


def check_workflow_files(workflows: list[Any]) -> int:
    yaml = _load_yaml()
    if yaml is None:
        return _without_yaml(require=bool(os.environ.get(REQUIRE_YAML_ENV)))
    findings: list[str] = []
    llvm_versions: dict[str, str] = {}
    presets = _load_production_presets()
    for path in workflows:
        document = yaml.safe_load(path.read_text(encoding="utf-8"))
        jobs = document.get("jobs", {})
        for finding in [
            *check_required_contexts(jobs),
            *check_release_writers(jobs),
            *check_prune_covers_savers(jobs),
            *check_master_only_jobs_queue(jobs),
            *check_production_off_pull_requests(jobs, presets),
        ]:
            findings.append(f"{path.name}: {finding}")
        llvm_version = (document.get("env") or {}).get("LLVM_VERSION")
        if llvm_version is not None:
            llvm_versions[path.name] = str(llvm_version)
    findings += check_llvm_versions(llvm_versions)
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

_FIXTURE_UNRELATED = {
    "heavy": _SKIPPED_ON_PRS,
    "lint": {"name": "not required", "needs": "heavy", "steps": []},
}


_RETIRED_IF = "if: github.event_name != 'pull_request' || github.head_ref == 'release'"
_TAG_ENV = "env:\n  X: ${{ startsWith(github.ref, 'refs/tags/v') && 'a' || 'b' }}"

_DRAFT_STEP = {"name": "Draft", "run": 'gh release create "$TAG" --draft'}
_FIXTURE_UNGATED_WRITER = {"do_release": {"if": "github.event_name != 'pull_request'", "steps": [_DRAFT_STEP]}}
_FIXTURE_GATED_WRITER = {"do_release": {"if": RELEASE_TAG_CONDITION, "steps": [_DRAFT_STEP]}}
_FIXTURE_WIDENED_WRITER = {
    "do_release": {"if": f"github.ref == 'refs/heads/master' || {RELEASE_TAG_CONDITION}", "steps": [_DRAFT_STEP]}
}
_FIXTURE_NEGATED_WRITER = {"do_release": {"if": f"!{RELEASE_TAG_CONDITION}", "steps": [_DRAFT_STEP]}}
_FIXTURE_CONJUNCT_WRITER = {
    "do_release": {"if": f"${{{{ github.repository == 'x/y' && {RELEASE_TAG_CONDITION} }}}}", "steps": [_DRAFT_STEP]}
}
_FIXTURE_API_WRITER = {
    "do_release": {"steps": [{"name": "Api", "run": "gh api repos/x/y/releases -f tag_name=v1"}]},
}
_FIXTURE_REUSABLE_WRITER = {"do_release": {"uses": "softprops/action-gh-release@v1"}}
_FIXTURE_SOFTPROPS = {
    "do_release": {"steps": [{"name": "Upload", "uses": "softprops/action-gh-release@v1"}]},
}
_FIXTURE_TAG_REQUIRED = {"windows": {"name": "Windows", "if": RELEASE_TAG_CONDITION, "steps": []}}


_CCACHE_STEP = {"uses": "hendrikmuhs/ccache-action@v1.2", "with": {"key": "k"}}
_PRUNE_STEP = {"run": "python3 scripts/prune-cache-generations.py --repo r --ref refs/heads/master"}
_FIXTURE_PRUNE_COVERS = {
    "windows": {"steps": [_CCACHE_STEP]},
    "linux": {"steps": [_CCACHE_STEP]},
    "lint": {"steps": []},
    "prune": {"needs": ["windows", "linux"], "steps": [_PRUNE_STEP]},
}
_FIXTURE_PRUNE_MISSES = {
    "windows": {"steps": [_CCACHE_STEP]},
    "linux": {"steps": [_CCACHE_STEP]},
    "prune": {"needs": "windows", "steps": [_PRUNE_STEP]},
}
_FIXTURE_SAVERS_UNPRUNED = {"windows": {"steps": [_CCACHE_STEP]}, "linux": {"steps": [_CCACHE_STEP]}}
_FIXTURE_NO_SAVERS = {"lint": {"steps": [{"run": "true"}]}}
_PACE_SAVE_STEP = {"uses": "actions/cache/save@v4", "with": {"path": "store", "key": "pace-store-windows-1-1"}}
_FIXTURE_PRUNE_COVERS_PACE = {
    "windows": {"steps": [_PACE_SAVE_STEP]},
    "prune": {"needs": ["windows"], "steps": [_PRUNE_STEP]},
}
_FIXTURE_PRUNE_MISSES_PACE = {
    "windows": {"steps": [_PACE_SAVE_STEP]},
    "macos": {"steps": [_CCACHE_STEP]},
    "prune": {"needs": ["macos"], "steps": [_PRUNE_STEP]},
}
_FIXTURE_OTHER_CACHE_SAVE = {
    "lint": {"steps": [{"uses": "actions/cache/save@v4", "with": {"path": "x", "key": "lint-tools-1"}}]}
}


# What the presets of cmake/presets/ say, reduced to the fields production_presets() reads.
_PRESET_DOCUMENTS = (
    {
        "configurePresets": [
            {"name": "contour-common", "hidden": True},
            {"name": "production", "hidden": True, "cacheVariables": {"CONTOUR_PRODUCTION_BUILD": "ON"}},
            {"name": "windows-common", "hidden": True, "inherits": "contour-common"},
            {"name": "msvc-release", "inherits": ["windows-common", "production"]},
            {"name": "msvc-ci", "inherits": ["windows-common"]},
            {"name": "derived-package", "inherits": "msvc-release"},
            # The option set by a preset itself, in either spelling CMake accepts, and turned off again.
            {"name": "flagged", "inherits": "windows-common", "cacheVariables": {"CONTOUR_PRODUCTION_BUILD": "true"}},
            {
                "name": "flagged-typed",
                "cacheVariables": {"CONTOUR_PRODUCTION_BUILD": {"type": "BOOL", "value": "1"}},
            },
            {"name": "opted-out", "inherits": "msvc-release", "cacheVariables": {"CONTOUR_PRODUCTION_BUILD": "OFF"}},
            # The first parent that says anything wins, as in CMake.
            {"name": "plain-first", "inherits": ["opted-out", "msvc-release"]},
        ],
        "buildPresets": [{"name": "msvc-release", "configurePreset": "msvc-release"}],
        "packagePresets": [{"name": "msvc-release-user", "configurePreset": "msvc-release"}],
        "testPresets": [{"name": "msvc-ci", "configurePreset": "msvc-ci"}],
    },
)
_PRODUCTION = frozenset({"msvc-release", "macos-package", "gcc-release"})


def _production_findings(jobs: dict[str, Any]) -> list[str]:
    return check_production_off_pull_requests(jobs, _PRODUCTION)


_FIXTURE_PRODUCTION_ON_PRS = {"windows": {"steps": [{"run": "cmake --preset msvc-release"}]}}
_FIXTURE_PRODUCTION_PRESET_IN_ENV = {
    "windows": {"env": {"CMAKE_PRESET": "msvc-release"}, "steps": [{"run": "cmake --preset ${{ env.CMAKE_PRESET }}"}]}
}
_FIXTURE_PRODUCTION_PRESET_AS_INPUT = {
    "macos": {"steps": [{"uses": "./.github/actions/macos-tests", "with": {"preset": "macos-package"}}]}
}
_FIXTURE_PRODUCTION_FLAG_SPELLINGS = {
    "no_space": {"steps": [{"run": "cmake --preset clang-release -DCONTOUR_PRODUCTION_BUILD=ON"}]},
    "typed": {"steps": [{"run": "cmake -B build -D CONTOUR_PRODUCTION_BUILD:BOOL=ON"}]},
    "numeric": {"steps": [{"run": "cmake -B build -DCONTOUR_PRODUCTION_BUILD=1"}]},
    "lowercase": {"steps": [{"run": "cmake -B build -DCONTOUR_PRODUCTION_BUILD=true"}]},
}
_FIXTURE_PRODUCTION_FLAG_OFF_SPELLINGS = {
    "off": {"steps": [{"run": "cmake -B build -DCONTOUR_PRODUCTION_BUILD:BOOL=OFF -DX=1"}]},
    "zero": {"steps": [{"run": "cmake -B build -DCONTOUR_PRODUCTION_BUILD=0"}]},
    "prefix": {"steps": [{"run": "cmake -B build -DMY_CONTOUR_PRODUCTION_BUILD=ON -DCONTOUR_PRODUCTION_BUILD=ONE"}]},
}
_FIXTURE_PRODUCTION_FLAG_ON_PRS = {
    "daemon": {"steps": [{"run": "cmake --preset clang-release -D CONTOUR_PRODUCTION_BUILD=ON"}]}
}
_FIXTURE_PRODUCTION_SCRIPT_ON_PRS = {"appimage_copy": {"steps": [{"run": "./scripts/build-appimage.sh"}]}}
_FIXTURE_PRODUCTION_GATED = {
    "windows_package": {**_SKIPPED_ON_PRS, "steps": [{"run": "cmake --build --preset=msvc-release"}]},
    "upload": {"needs": "windows_package", "steps": [{"run": "cmake --preset gcc-release"}]},
}
_FIXTURE_PLAIN_ON_PRS = {
    "windows": {"env": {"CMAKE_PRESET": "msvc-ci"}, "steps": [{"run": "cmake --preset msvc-ci"}]},
    "linux": {"steps": [{"run": "cmake --preset clang-release -DCONTOUR_PRODUCTION_BUILD=OFF"}]},
}
_FIXTURE_PRODUCTION_EXCEPTIONS = {
    "package_daemon_for_Ubuntu": {"steps": [{"run": "cmake --preset clang-release -D CONTOUR_PRODUCTION_BUILD=ON"}]},
    "package_for_AppImage": {"steps": [{"run": "./scripts/build-appimage.sh"}]},
}
_FIXTURE_PRODUCTION_EXCEPTION_STALE = {
    "package_daemon_for_Ubuntu": {"steps": [{"run": "cmake --preset clang-release"}]},
    "package_for_AppImage": {**_SKIPPED_ON_PRS, "steps": [{"run": "./scripts/build-appimage.sh"}]},
}


_QUEUED = {"group": "${{ github.ref }}-freebsd", "cancel-in-progress": False}
_FIXTURE_MASTER_ONLY_QUEUED = {
    "freebsd": {**_SKIPPED_ON_PRS, "concurrency": _QUEUED},
    "packaging": {
        **_SKIPPED_ON_PRS,
        "strategy": {"matrix": {"os_version": ["26.04"]}},
        "concurrency": {"group": "${{ github.ref }}-packaging-${{ strategy.job-index }}"},
    },
    "windows": {"steps": []},
    "do_release": {"if": RELEASE_TAG_CONDITION, "steps": []},
}
_FIXTURE_MASTER_ONLY_UNQUEUED = {"freebsd": dict(_SKIPPED_ON_PRS)}
_FIXTURE_MASTER_ONLY_SHARED_GROUP = {"freebsd": {**_SKIPPED_ON_PRS, "concurrency": {"group": "${{ github.ref }}"}}}
_FIXTURE_MASTER_ONLY_REFLESS = {"freebsd": {**_SKIPPED_ON_PRS, "concurrency": {"group": "freebsd"}}}
_FIXTURE_MASTER_ONLY_LEGS_SHARE = {
    "packaging": {
        **_SKIPPED_ON_PRS,
        "strategy": {"matrix": {"os_version": ["24.04", "26.04"]}},
        "concurrency": {"group": "${{ github.ref }}-packaging"},
    }
}
_FIXTURE_MASTER_ONLY_CANCELS = {
    "freebsd": {**_SKIPPED_ON_PRS, "concurrency": {**_QUEUED, "cancel-in-progress": True}}
}


def self_test() -> int:
    cases = (
        ("success-only gate over a PR-skipped need fires", check_required_contexts, _FIXTURE_BROKEN, 2),
        ("always() plus accepting 'skipped' passes", check_required_contexts, _FIXTURE_FIXED, 0),
        ("a required job gated off pull requests fires", check_required_contexts, _FIXTURE_GATED_REQUIRED, 1),
        (
            "a required job needing a skipped job without always() fires",
            check_required_contexts,
            _FIXTURE_NO_ALWAYS,
            1,
        ),
        ("a job that is not a required context may be gated", check_required_contexts, _FIXTURE_UNRELATED, 0),
        ("a required job gated on a release tag fires", check_required_contexts, _FIXTURE_TAG_REQUIRED, 1),
        ("the retired head_ref == 'release' condition fires", check_text, _RETIRED_IF, 1),
        ("the retired condition fires inside a comment", check_text, f"# {_RETIRED_IF}", 1),
        ("the retired head_ref != 'release' condition fires", check_text, "if: github.head_ref != 'release'", 1),
        ("refs/tags/ spelled another way fires", check_text, "if: startsWith(github.ref, 'refs/tags/')", 1),
        (
            "the canonical release-tag condition passes, in an if and in an expression",
            check_text,
            f"if: {RELEASE_TAG_CONDITION}\n{_TAG_ENV}",
            0,
        ),
        ("an ungated job creating a release fires", check_release_writers, _FIXTURE_UNGATED_WRITER, 1),
        ("a release-tag job creating a release passes", check_release_writers, _FIXTURE_GATED_WRITER, 0),
        ("an ungated softprops step fires", check_release_writers, _FIXTURE_SOFTPROPS, 1),
        ("a release-tag condition widened with || fires", check_release_writers, _FIXTURE_WIDENED_WRITER, 1),
        ("a negated release-tag condition fires", check_release_writers, _FIXTURE_NEGATED_WRITER, 1),
        ("a release-tag condition among && conjuncts passes", check_release_writers, _FIXTURE_CONJUNCT_WRITER, 0),
        ("an ungated gh api releases step fires", check_release_writers, _FIXTURE_API_WRITER, 1),
        ("an ungated job-level uses fires", check_release_writers, _FIXTURE_REUSABLE_WRITER, 1),
        ("a prune job needing every cache-saving job passes", check_prune_covers_savers, _FIXTURE_PRUNE_COVERS, 0),
        ("a cache-saving job the prune does not wait for fires", check_prune_covers_savers, _FIXTURE_PRUNE_MISSES, 1),
        ("cache-saving jobs without a prune job fire", check_prune_covers_savers, _FIXTURE_SAVERS_UNPRUNED, 1),
        ("a workflow that saves no compiler cache passes", check_prune_covers_savers, _FIXTURE_NO_SAVERS, 0),
        ("a prune job needing the Pace store's saver passes", check_prune_covers_savers, _FIXTURE_PRUNE_COVERS_PACE, 0),
        (
            "a Pace store saver the prune does not wait for fires",
            check_prune_covers_savers,
            _FIXTURE_PRUNE_MISSES_PACE,
            1,
        ),
        ("a cache/save of another key is no compiler cache", check_prune_covers_savers, _FIXTURE_OTHER_CACHE_SAVE, 0),
        ("a production preset on pull requests fires", _production_findings, _FIXTURE_PRODUCTION_ON_PRS, 1),
        (
            "a production preset named in env on pull requests fires",
            _production_findings,
            _FIXTURE_PRODUCTION_PRESET_IN_ENV,
            1,
        ),
        (
            "a production preset passed to an action on pull requests fires",
            _production_findings,
            _FIXTURE_PRODUCTION_PRESET_AS_INPUT,
            1,
        ),
        ("CONTOUR_PRODUCTION_BUILD=ON on pull requests fires", _production_findings, _FIXTURE_PRODUCTION_FLAG_ON_PRS, 1),
        (
            "CONTOUR_PRODUCTION_BUILD set true without a space, typed, as 1 or lowercase fires",
            _production_findings,
            _FIXTURE_PRODUCTION_FLAG_SPELLINGS,
            4,
        ),
        (
            "CONTOUR_PRODUCTION_BUILD set false, or a longer name, passes",
            _production_findings,
            _FIXTURE_PRODUCTION_FLAG_OFF_SPELLINGS,
            0,
        ),
        ("a script that builds production on pull requests fires", _production_findings, _FIXTURE_PRODUCTION_SCRIPT_ON_PRS, 1),
        ("production builds gated off pull requests pass", _production_findings, _FIXTURE_PRODUCTION_GATED, 0),
        ("plain builds on pull requests pass", _production_findings, _FIXTURE_PLAIN_ON_PRS, 0),
        ("the documented exceptions pass on pull requests", _production_findings, _FIXTURE_PRODUCTION_EXCEPTIONS, 0),
        (
            "an exception that no longer builds production, or no longer runs on pull requests, fires",
            _production_findings,
            _FIXTURE_PRODUCTION_EXCEPTION_STALE,
            2,
        ),
        (
            "production presets set CONTOUR_PRODUCTION_BUILD on, themselves or by inheritance, and the presets built on them",
            lambda documents: sorted(
                # A one-shot iterator, as the preset files are read.
                production_presets(iter(documents))
                ^ {"msvc-release", "derived-package", "msvc-release-user", "flagged", "flagged-typed"}
            ),
            _PRESET_DOCUMENTS,
            0,
        ),
        ("master-only jobs queueing per ref, job and leg pass", check_master_only_jobs_queue, _FIXTURE_MASTER_ONLY_QUEUED, 0),
        ("a master-only job without a concurrency group fires", check_master_only_jobs_queue, _FIXTURE_MASTER_ONLY_UNQUEUED, 1),
        (
            "a master-only job sharing one group with other jobs fires",
            check_master_only_jobs_queue,
            _FIXTURE_MASTER_ONLY_SHARED_GROUP,
            1,
        ),
        (
            "a master-only job whose group ignores the ref fires",
            check_master_only_jobs_queue,
            _FIXTURE_MASTER_ONLY_REFLESS,
            1,
        ),
        (
            "master-only matrix legs sharing one group fire",
            check_master_only_jobs_queue,
            _FIXTURE_MASTER_ONLY_LEGS_SHARE,
            1,
        ),
        (
            "a master-only job cancelling its running predecessor fires",
            check_master_only_jobs_queue,
            _FIXTURE_MASTER_ONLY_CANCELS,
            1,
        ),
        ("one workflow setting LLVM_VERSION passes", check_llvm_versions, {"build.yml": "22"}, 0),
        (
            "two workflows agreeing on LLVM_VERSION pass",
            check_llvm_versions,
            {"build.yml": "22", "clang-tidy.yml": "22"},
            0,
        ),
        (
            "two workflows disagreeing on LLVM_VERSION fire",
            check_llvm_versions,
            {"build.yml": "22", "clang-tidy.yml": "21"},
            1,
        ),
        ("no workflow setting LLVM_VERSION passes", check_llvm_versions, {}, 0),
    )
    failures = []
    for description, rule, fixture, expected in cases:
        found = rule(fixture)
        if len(found) != expected:
            failures.append(f"{description}: expected {expected} finding(s), got {found}")
    if _without_yaml(require=True) != 1:
        failures.append("a missing PyYAML must fail when it is required")
    if _without_yaml(require=False) != 0:
        failures.append("a missing PyYAML must skip when it is not required")
    if _exit_code(0, ["finding"]) != 1 or _exit_code(0, []) != 0 or _exit_code(2, []) != 2:
        failures.append("text findings must fail the run whatever the tool did")
    # The download retry policy lives in the shared tool cache; this is the self-test that runs it.
    failures.extend(f"tool download: {failure}" for failure in download_self_test())
    for failure in failures:
        print(f"check-workflows self-test: FAILED: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"check-workflows self-test: {len(cases) + 7} cases OK.")
    return 0


def _exit_code(tool_exit_code: int, text_findings: list[str]) -> int:
    """The process exit code: the text rules fail the run even when the tool cannot run."""
    return max(tool_exit_code, 1 if text_findings else 0)


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

    # The text rules need no YAML and no actionlint, so they run (and are reported) first.
    text_findings = [
        f"{path.name}: {finding}" for path in workflows for finding in check_text(path.read_text(encoding="utf-8"))
    ]
    for finding in text_findings:
        print(f"check-workflows: {finding}", file=sys.stderr)

    try:
        actionlint = resolve(ACTIONLINT)
    except SystemExit as unavailable:
        # The tool is absent and not required: skip it, but not the text rules' findings.
        return _exit_code(unavailable.code if isinstance(unavailable.code, int) else 0, text_findings)

    arguments: list[str] = []
    for message in IGNORED_MESSAGES:
        arguments += ["-ignore", message]
    arguments += [str(path.relative_to(REPO_ROOT)) for path in workflows]

    exit_code = run(actionlint, arguments)
    if exit_code != 0:
        return _exit_code(exit_code, text_findings)

    print(f"actionlint: workflows OK ({len(workflows)} files).")
    return _exit_code(check_workflow_files(workflows), text_findings)


if __name__ == "__main__":
    sys.exit(main())
