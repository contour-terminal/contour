#! /usr/bin/env python3
"""Plans a release's winget submission, finishes the manifests Komac renders for it, and checks them.

    winget-release.py plan --tag TAG --out PATH             read the published release, write the plan
    winget-release.py finalize --plan P --render R --zip Z --out DIR
                                                            split Komac's printed manifests, finish them, write them
    winget-release.py check --plan PATH SOURCE              check the installer manifest against the plan
    winget-release.py existing-pr --version V               the open or merged pull request for a version, if any
    winget-release.py submitted-pr --log L --version V      the pull request Komac opened
    winget-release.py                                       check the UpgradeCodes against the packaging sources
    winget-release.py --self-test                           check the rules against inline samples

A release's Windows installers are the rows of INSTALLERS: the per-machine MSI, the per-user MSI and the
portable ZIP. `plan` finds them among the release's assets and refuses a release whose assets do not agree on
one version.

Komac 2.17.1 cannot write what it renders without a terminal (`--dry-run` prints the three manifests, which
is what CI captures), and it cannot finish two things for a ZIP of several executables or for this project:

- It asks which file of the ZIP runs, only when it can see a terminal, so a render in CI has `InstallerType: zip`
  and no nested installer, which winget rejects. `finalize` adds `NestedInstallerType: portable` and the
  `NestedInstallerFiles` path it finds in the ZIP itself, and checks the ZIP's SHA-256 against Komac's.
- It adds `InstallerLocale: en-US` to the MSIs, not to the ZIP. Without one the ZIP wins a plain install on a PC
  whose languages include no English (winget-cli LocaleComparator ranks first). `finalize` adds it.
- It carries `Dependencies` (the VC++ runtime) over from the newest version in winget-pkgs. Contour ships the
  runtime app-local, and a dependency would make a per-user install ask for an administrator. `finalize` removes it.

`check` runs on the files `finalize` wrote, and those are the files `komac submit` uploads: nothing is rendered twice.
It asserts what winget will see (exactly one installer per planned URL, each one's type, scope, locale, UpgradeCode,
nested file, distinct ProductCodes, no dependencies) and which installer winget picks for `--scope user`,
`--scope machine` and a plain install (SELECTIONS), by modelling winget-cli's choice.

Python rather than shell, and stdlib only (PyYAML is imported by `check` alone), so that CTest runs the
self-test and the source check on every platform.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
IDENTIFIER = "contour-terminal.Contour"
LOCALE = "en-US"
WINGET_PKGS = "microsoft/winget-pkgs"


@dataclass(frozen=True)
class InstallerRow:
    """One Windows installer of a release, as winget has to see it."""

    label: str
    asset: re.Pattern[str]  # fullmatch on the asset name; captures the version
    installer_type: str
    scope: str | None  # None: the manifest says none (a portable install has no scope)
    upgrade_code: str | None  # upper case, no braces
    nested_file_suffix: str | None  # the path of the program inside a ZIP, from its top folder on


INSTALLERS: tuple[InstallerRow, ...] = (
    InstallerRow(
        "per-machine MSI",
        re.compile(r"contour-(?P<version>\d+\.\d+\.\d+\.\d+)-win64\.msi"),
        "wix",
        "machine",
        "0E736497-2B72-4117-95E9-54EC6D000000",
        None,
    ),
    InstallerRow(
        "per-user MSI",
        re.compile(r"contour-(?P<version>\d+\.\d+\.\d+\.\d+)-win64-user\.msi"),
        "wix",
        "user",
        "AFF353C7-C33A-474C-B9C7-EDD160677BC9",
        None,
    ),
    InstallerRow(
        "portable ZIP",
        re.compile(r"contour-(?P<version>\d+\.\d+\.\d+\.\d+)-win64\.zip"),
        "zip",
        None,
        None,
        "bin/contour.exe",
    ),
)

# What `winget install contour-terminal.Contour [--scope S]` picks, by scope (None: no --scope). The docs promise
# exactly this table (docs/install.md); `check` models winget-cli's choice (select_installer) and pins it.
SELECTIONS: tuple[tuple[str | None, str], ...] = (
    ("user", "per-user MSI"),
    ("machine", "per-machine MSI"),
    (None, "per-user MSI"),
)

# winget-cli (master afc9a471, src/AppInstallerCommonCore/Manifest/):
# the default installer type order when the user set no preference, best first
# (ManifestComparator.cpp:232-241, InstallerTypeComparator::Create).
INSTALLER_TYPE_ORDER = ("msstore", "msix", "msi", "wix", "burn", "nullsoft", "inno", "exe", "portable")
# the types that stay applicable under any `--scope` (ManifestCommon.cpp:940-947,
# DoesInstallerTypeIgnoreScopeFromManifest).
SCOPE_AGNOSTIC_TYPES = ("font", "portable", "msix", "msstore")
# the scope preference setting `installBehavior.preferences.scope` defaults to user (UserSettings.h:206).
DEFAULT_SCOPE_PREFERENCE = "user"

# Where the UpgradeCodes have to be declared (relative to the repository root).
UPGRADE_CODE_SOURCES = ("src/contour/CMakeLists.txt", "src/contour/packaging", "cmake")


@dataclass(frozen=True)
class Plan:
    """What to submit: the release's tag and version, its notes, and the installer URL per label."""

    tag: str
    version: str
    release_notes_url: str
    installers: dict[str, str]


def make_plan(
    tag: str, is_draft: bool, assets: Sequence[tuple[str, str]], release_url: str
) -> tuple[Plan | None, list[str]]:
    """Builds the plan from a release's (name, download url) assets, or says what is wrong with it."""
    problems: list[str] = []
    if is_draft:
        problems.append(f"release {tag} is not published")
    urls: dict[str, str] = {}
    versions: dict[str, str] = {}  # asset name -> version
    for row in INSTALLERS:
        matches = [(name, m, url) for name, url in assets if (m := row.asset.fullmatch(name))]
        if not matches:
            problems.append(f"no asset for the {row.label}")
        elif len(matches) > 1:
            problems.append(f"{len(matches)} assets match the {row.label}: {', '.join(name for name, _, _ in matches)}")
        else:
            name, match, url = matches[0]
            urls[row.label] = url
            versions[name] = match.group("version")
    if len(set(versions.values())) > 1:
        problems.append("assets carry different versions: " + ", ".join(f"{n} ({v})" for n, v in sorted(versions.items())))
    elif versions:
        version = next(iter(versions.values()))
        release = tag.removeprefix("v")
        if release != version and release != version.rpartition(".")[0]:
            problems.append(f"tag {tag} does not match the assets' version {version}: {', '.join(sorted(versions))}")
    if problems:
        return None, problems
    return Plan(tag, next(iter(versions.values())), release_url, urls), []


def _get(installer: dict[str, Any], manifest: dict[str, Any], key: str) -> Any:
    """A field of an installer, or else of the manifest's root: Komac hoists what all installers share."""
    return installer.get(key, manifest.get(key))


def _effective_type(installer: dict[str, Any], manifest: dict[str, Any]) -> str:
    """The type winget ranks an installer by: a zip's nested type, else its own."""
    own = _get(installer, manifest, "InstallerType") or ""
    return (_get(installer, manifest, "NestedInstallerType") or own) if own == "zip" else own


def _first_better_by_scope(first: str | None, second: str | None, preference: str) -> str | None:
    """ScopeComparator::IsFirstBetter (ManifestComparator.cpp:515-524): strong for the preferred scope over the other
    scope, weak over an installer with none."""
    if first == preference and second != preference:
        return "weak" if second is None else "strong"
    return None


def _first_better_by_type(first: str, first_base: str, second: str, second_base: str) -> str | None:
    """InstallerTypeComparator::IsFirstBetter (ManifestComparator.cpp:289-315): weak for the type that comes first in
    the default order, where only one of the two is in it at that position."""
    for kind in INSTALLER_TYPE_ORDER:
        first_is = kind in (first, first_base)
        second_is = kind in (second, second_base)
        if first_is and second_is:
            return None
        if first_is != second_is:
            return "weak" if first_is else None
    return None


def select_installer(manifest: dict[str, Any], scope: str | None) -> dict[str, Any] | None:
    """Models winget-cli's choice among a manifest's installers on an x64 PC with default settings.

    ManifestComparator::GetPreferredInstaller (ManifestComparator.cpp:~830): walk the installers in manifest order, keep
    the first applicable one, replace it by a later one only if that one is better (IsFirstBetter, :~800-870: the
    comparators in order, the first strong result in either direction decides, else the first weak one).
    `--scope S`: ScopeComparator::IsApplicable (:488-503) drops an installer whose scope is not S, but keeps one whose
    type ignores scope. Comparators, in order (:766-779): locale (all installers carry the same, which `check`
    asserts), scope (preference `user` by default), architecture (all x64), installer type.
    """
    best: dict[str, Any] | None = None
    for installer in manifest.get("Installers", []):
        kind = _effective_type(installer, manifest)
        if scope is not None and _get(installer, manifest, "Scope") != scope and kind not in SCOPE_AGNOSTIC_TYPES:
            continue
        if best is None or _is_first_better(installer, best, manifest):
            best = installer
    return best


def _is_first_better(first: dict[str, Any], second: dict[str, Any], manifest: dict[str, Any]) -> bool:
    """ManifestComparator::IsFirstBetter."""
    first_scope, second_scope = _get(first, manifest, "Scope"), _get(second, manifest, "Scope")
    first_type, second_type = _effective_type(first, manifest), _effective_type(second, manifest)
    first_base, second_base = _get(first, manifest, "InstallerType") or "", _get(second, manifest, "InstallerType") or ""
    comparisons = (
        (
            _first_better_by_scope(first_scope, second_scope, DEFAULT_SCOPE_PREFERENCE),
            _first_better_by_scope(second_scope, first_scope, DEFAULT_SCOPE_PREFERENCE),
        ),
        (
            _first_better_by_type(first_type, first_base, second_type, second_base),
            _first_better_by_type(second_type, second_base, first_type, first_base),
        ),
    )
    weak: bool | None = None
    for forward, reverse in comparisons:
        if forward == "strong":
            return True
        if reverse == "strong":
            return False
        if weak is None:
            if forward == "weak":
                weak = True
            elif reverse == "weak":
                weak = False
    return bool(weak)


def check_manifest(plan: Plan, manifest: dict[str, Any]) -> list[str]:
    """Returns what is wrong with the finished installer manifest for this plan; empty when it is right."""
    problems: list[str] = []
    if str(manifest.get("PackageVersion")) != plan.version:
        problems.append(f"PackageVersion is {manifest.get('PackageVersion')}, expected {plan.version}")
    # The VC++ runtime ships app-local: a dependency makes a per-user install ask for an administrator.
    if "Dependencies" in manifest or any("Dependencies" in installer for installer in manifest.get("Installers", [])):
        problems.append("Dependencies is present; the runtime ships with Contour, and a dependency makes a per-user install ask for an administrator")

    installers: list[dict[str, Any]] = manifest.get("Installers", [])
    urls = [installer.get("InstallerUrl") for installer in installers]
    planned = set(plan.installers.values())
    problems += [f"installer {url} is not in the plan" for url in dict.fromkeys(urls) if url not in planned]
    problems += [f"installer {url} is listed {urls.count(url)} times" for url in dict.fromkeys(urls) if urls.count(url) > 1]

    found: dict[str, dict[str, Any]] = {}
    for row in INSTALLERS:
        url = plan.installers.get(row.label)
        installer = next((i for i in installers if i.get("InstallerUrl") == url), None)
        if installer is None:
            problems.append(f"{row.label}: no installer for {url}")
            continue
        found[row.label] = installer
        kind = _get(installer, manifest, "InstallerType")
        if kind != row.installer_type:
            problems.append(f"{row.label}: InstallerType is {kind}, expected {row.installer_type}")
        scope = _get(installer, manifest, "Scope")
        if row.scope is None:
            if scope is not None:
                problems.append(f"{row.label}: Scope is {scope}, expected none")
        elif scope is None:
            problems.append(f"{row.label}: Scope is missing, expected {row.scope}")
        elif scope != row.scope:
            problems.append(f"{row.label}: Scope is {scope}, expected {row.scope}")
        # One locale for all, so that no installer wins on locale (winget-cli's LocaleComparator ranks first).
        locale = _get(installer, manifest, "InstallerLocale")
        if locale != LOCALE:
            problems.append(f"{row.label}: InstallerLocale is {locale}, expected {LOCALE}")
        if row.upgrade_code is not None:
            entries = _get(installer, manifest, "AppsAndFeaturesEntries") or []
            codes = [str(entry.get("UpgradeCode", "")) for entry in entries]
            expected = "{" + row.upgrade_code + "}"
            if not entries:
                problems.append(f"{row.label}: AppsAndFeaturesEntries is missing, expected UpgradeCode {expected}")
            elif expected.upper() not in (code.upper() for code in codes):
                problems.append(f"{row.label}: UpgradeCode {', '.join(codes)} expected {expected}")
        if row.nested_file_suffix is not None:
            nested = _get(installer, manifest, "NestedInstallerType")
            if nested != "portable":
                problems.append(f"{row.label}: NestedInstallerType is {nested}, expected portable")
            files = _get(installer, manifest, "NestedInstallerFiles") or []
            paths = [str(entry.get("RelativeFilePath", "")) for entry in files]
            if len(paths) != 1 or not paths[0].endswith("/" + row.nested_file_suffix):
                problems.append(f"{row.label}: no nested file ending in {row.nested_file_suffix} (NestedInstallerFiles has {paths})")

    msi_codes = [str(i.get("ProductCode", "")).upper() for label, i in found.items() if INSTALLERS_BY_LABEL[label].upgrade_code]
    if len(msi_codes) != len(set(msi_codes)) or "" in msi_codes:
        problems.append("the installers do not each have their own ProductCode")

    if len(found) == len(INSTALLERS):
        for scope, label in SELECTIONS:
            picked = select_installer(manifest, scope)
            if picked is not found[label]:
                shown = "nothing" if picked is None else str(picked.get("InstallerUrl"))
                problems.append(
                    f"winget picks {shown} for {'--scope ' + scope if scope else 'a plain install'}, expected the {label}"
                )
    return problems


INSTALLERS_BY_LABEL = {row.label: row for row in INSTALLERS}


def undeclared_upgrade_codes(codes: Sequence[str], source_texts: Sequence[str]) -> list[str]:
    """Returns the UpgradeCodes that no source text declares (any case, braces or not)."""
    haystack = "\n".join(source_texts).upper().replace("{", "").replace("}", "")
    return [code for code in codes if code.upper().replace("{", "").replace("}", "") not in haystack]


def read_sources(root: Path) -> list[str]:
    """Returns the text of the files that declare the UpgradeCodes."""
    texts: list[str] = []
    for relative in UPGRADE_CODE_SOURCES:
        path = root / relative
        files = [path] if path.is_file() else sorted(p for p in path.rglob("*") if p.is_file())
        for file in files:
            texts.append(file.read_text(encoding="utf-8", errors="ignore"))
    return texts


# --- the manifests Komac printed ------------------------------------------------------------------------------


KOMAC_HEADER = re.compile(r"(?m)^(?=# Created with komac)")
MANIFEST_TYPE = re.compile(r"(?m)^ManifestType: (\w+)\s*$")


def split_documents(text: str) -> dict[str, str]:
    """Splits what Komac printed into its manifests, by ManifestType (installer, version, defaultLocale)."""
    documents: dict[str, str] = {}
    for part in KOMAC_HEADER.split(text.replace("\r\n", "\n")):
        match = MANIFEST_TYPE.search(part)
        if match:
            if match.group(1) in documents:
                raise ValueError(f"Komac printed two {match.group(1)} manifests")
            documents[match.group(1)] = part.rstrip("\n") + "\n"
    return documents


def installer_manifest_text(source: Path) -> str:
    """Returns the installer manifest's YAML text from a manifest directory or Komac's printed output."""
    if source.is_dir():
        files = sorted(source.rglob("*.installer.yaml"))
        if len(files) != 1:
            raise ValueError(f"expected one *.installer.yaml under {source}, found {len(files)}")
        return files[0].read_text(encoding="utf-8")
    documents = split_documents(source.read_text(encoding="utf-8"))
    if "installer" not in documents:
        raise ValueError(f"expected an installer manifest in {source}")
    return documents["installer"]


def finish_installer_manifest(text: str, zip_url: str, nested_path: str) -> str:
    """Finishes Komac's installer manifest: no Dependencies; the ZIP gets InstallerLocale and its nested installer."""
    lines = text.split("\n")
    # Dependencies: the key and its indented block.
    out: list[str] = []
    skipping = False
    for line in lines:
        if line.startswith("Dependencies:"):
            skipping = True
            continue
        if skipping and (line.startswith(" ") or line.startswith("\t")):
            continue
        skipping = False
        out.append(line)
    lines = out
    # The ZIP's entry of the Installers list: from its "- " line to the next one (or the end of the list).
    try:
        start = next(i for i, line in enumerate(lines) if line == "Installers:") + 1
    except StopIteration as error:
        raise ValueError("the installer manifest has no Installers") from error
    entries: list[tuple[int, int]] = []
    index = start
    while index < len(lines) and (lines[index].startswith("-") or lines[index].startswith(" ")):
        begin = index
        index += 1
        while index < len(lines) and lines[index].startswith(" "):
            index += 1
        entries.append((begin, index))
    wanted = [(b, e) for b, e in entries if any(line.strip() == f"InstallerUrl: {zip_url}" or line.strip() == f"- InstallerUrl: {zip_url}" for line in lines[b:e])]
    if len(wanted) != 1:
        raise ValueError(f"expected one installer for {zip_url}, found {len(wanted)}")
    begin, end = wanted[0]
    body = "\n".join(lines[begin:end])
    additions: list[str] = []
    if "InstallerLocale:" not in body:
        additions.append(f"  InstallerLocale: {LOCALE}")
    if "NestedInstallerType:" not in body:
        additions.append("  NestedInstallerType: portable")
    if "NestedInstallerFiles:" not in body:
        additions += ["  NestedInstallerFiles:", f"  - RelativeFilePath: {nested_path}"]
    lines[end:end] = additions
    return "\n".join(lines)


def find_nested_file(zip_path: Path, suffix: str) -> str:
    """Returns the one path inside a ZIP that ends in /suffix: the program the portable install runs."""
    with zipfile.ZipFile(zip_path) as archive:
        names = [name for name in archive.namelist() if name.endswith("/" + suffix)]
    if len(names) != 1:
        raise ValueError(f"expected one file ending in /{suffix} in {zip_path}, found {len(names)}")
    return names[0]


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def manifest_file_names(documents: dict[str, str]) -> dict[str, str]:
    """The file name winget-pkgs uses for each manifest: <id>.yaml, <id>.installer.yaml, <id>.locale.<locale>.yaml."""
    locale = re.search(r"(?m)^PackageLocale: (\S+)\s*$", documents.get("defaultLocale", ""))
    names = {"version": f"{IDENTIFIER}.yaml", "installer": f"{IDENTIFIER}.installer.yaml"}
    if locale:
        names["defaultLocale"] = f"{IDENTIFIER}.locale.{locale.group(1)}.yaml"
    return names


# --- pull requests --------------------------------------------------------------------------------------------


PULL_REQUEST_URL = re.compile(r"https://github\.com/microsoft/winget-pkgs/pull/\d+")


def pick_pull_request(pulls: Sequence[dict[str, Any]], version: str) -> str | None:
    """The URL of an open or merged pull request that is for this version (a whole-word match in its title, as Komac's
    own check does); a closed, unmerged one does not count, or the version could never be submitted again."""
    pattern = re.compile(rf"(?<![\w.]){re.escape(IDENTIFIER)}\b.*(?<![\w.]){re.escape(version)}(?![\w.])")
    for pull in pulls:
        if pull.get("state") != "CLOSED" and pattern.search(str(pull.get("title", ""))):
            return str(pull["url"])
    return None


def pull_request_in_output(text: str) -> str | None:
    """The pull request URL Komac printed when it opened one."""
    match = PULL_REQUEST_URL.search(text)
    return match.group(0) if match else None


def search_pull_requests(version: str) -> str | None:
    """Looks the pull request for a version up on GitHub."""
    result = subprocess.run(
        ["gh", "pr", "list", "-R", WINGET_PKGS, "--state", "all", "--search", f"{IDENTIFIER} {version} in:title",
         "--json", "url,title,state"],
        capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"gh pr list failed: {result.stderr.strip()}")
    return pick_pull_request(json.loads(result.stdout), version)


# --- command line ---------------------------------------------------------------------------------------------


def write_outputs(outputs: dict[str, str]) -> None:
    """Appends step outputs to $GITHUB_OUTPUT, or prints them."""
    lines = "".join(f"{key}={value}\n" for key, value in outputs.items())
    target = os.environ.get("GITHUB_OUTPUT")
    if target:
        with open(target, "a", encoding="utf-8") as stream:
            stream.write(lines)
    else:
        print(lines, end="")


def run_plan(tag: str, out: Path) -> int:
    """Asks GitHub for the release and writes the plan (and the step outputs)."""
    result = subprocess.run(
        ["gh", "release", "view", tag, "--json", "assets,url,isDraft,isPrerelease"],
        capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        print(f"winget-release: gh release view {tag} failed: {result.stderr.strip()}", file=sys.stderr)
        return 1
    release = json.loads(result.stdout)
    assets = [(a["name"], a["url"]) for a in release["assets"]]
    plan, problems = make_plan(tag, bool(release["isDraft"]), assets, release["url"])
    if plan is None:
        for problem in problems:
            print(f"winget-release: {problem}", file=sys.stderr)
        return 1
    out.write_text(json.dumps(asdict(plan), indent=2) + "\n", encoding="utf-8")
    write_outputs({
        "version": plan.version,
        "urls": " ".join(plan.installers[row.label] for row in INSTALLERS),
        "zip_url": plan.installers["portable ZIP"],
        "release_notes_url": plan.release_notes_url,
        "prerelease": str(bool(release["isPrerelease"])).lower(),
    })
    return 0


def run_finalize(plan_path: Path, render: Path, zip_path: Path, out: Path) -> int:
    """Writes the three manifests of a render, finished, to the folder winget-pkgs lays them out in."""
    plan = Plan(**json.loads(plan_path.read_text(encoding="utf-8")))
    try:
        documents = split_documents(render.read_text(encoding="utf-8"))
        names = manifest_file_names(documents)
        missing = [kind for kind in ("version", "installer", "defaultLocale") if kind not in documents]
        if missing:
            raise ValueError(f"Komac printed no {', '.join(missing)} manifest")
        row = next(r for r in INSTALLERS if r.nested_file_suffix)
        nested = find_nested_file(zip_path, row.nested_file_suffix)
        documents["installer"] = finish_installer_manifest(documents["installer"], plan.installers[row.label], nested)
        # The ZIP Komac hashed is the ZIP that was inspected.
        sums = re.findall(rf"(?m)^  InstallerUrl: {re.escape(plan.installers[row.label])}\n  InstallerSha256: (\w+)", documents["installer"])
        if sums and sums[0].upper() != file_sha256(zip_path):
            raise ValueError(f"the ZIP has SHA-256 {file_sha256(zip_path)}, Komac hashed {sums[0]}")
    except ValueError as error:
        print(f"winget-release: {error}", file=sys.stderr)
        return 1
    directory = out
    directory.mkdir(parents=True, exist_ok=True)
    for kind, name in names.items():
        (directory / name).write_text(documents[kind], encoding="utf-8", newline="\n")
    print(f"winget-release: wrote {len(names)} manifests of {plan.version} to {directory}")
    return 0


def run_check(plan_path: Path, source: Path) -> int:
    """Checks the installer manifest against the plan file."""
    import yaml  # PyYAML: only this command needs it

    plan = Plan(**json.loads(plan_path.read_text(encoding="utf-8")))
    try:
        manifest = yaml.safe_load(installer_manifest_text(source))
    except ValueError as error:
        print(f"winget-release: {error}", file=sys.stderr)
        return 1
    problems = check_manifest(plan, manifest)
    for problem in problems:
        print(f"winget-release: {problem}", file=sys.stderr)
    if not problems:
        print(f"winget-release: the manifest for {plan.version} is what the plan promises.")
    return 1 if problems else 0


def run_sources() -> int:
    """Checks that the packaging sources declare every UpgradeCode the winget rows expect."""
    codes = [r.upgrade_code for r in INSTALLERS if r.upgrade_code]
    missing = undeclared_upgrade_codes(codes, read_sources(REPO_ROOT))
    for code in missing:
        print(f"winget-release: UpgradeCode {code} is not declared in the packaging sources", file=sys.stderr)
    if not missing:
        print(f"winget-release: {len(codes)} UpgradeCodes declared in the packaging sources.")
    return 1 if missing else 0


# --- self-test ------------------------------------------------------------------------------------------------

_RELEASE = "https://github.com/contour-terminal/contour/releases/download/v0.9.0"
_NAMES = (
    "contour-0.9.0.9500-win64.msi",
    "contour-0.9.0.9500-win64-user.msi",
    "contour-0.9.0.9500-win64.zip",
    "contour-0.9.0.9500-ubuntu26.04-amd64.deb",
    "contour-0.9.0.9500-macos-arm64.dmg",
)
_NESTED = "Contour-0.9.0.9500-win64/bin/contour.exe"

# Komac 2.17.1's `--dry-run` output for the 0.9 packages (three installers: ZIP, per-user MSI, per-machine MSI),
# re-versioned. It has Dependencies, no nested installer and no locale for the ZIP.
_RENDER = f"""Latest version of contour-terminal.Contour: 0.7.0.8982
# Created with komac v2.17.1
# yaml-language-server: $schema=https://aka.ms/winget-manifest.installer.1.12.0.schema.json

PackageIdentifier: contour-terminal.Contour
PackageVersion: 0.9.0.9500
InstallModes:
- interactive
- silent
- silentWithProgress
UpgradeBehavior: install
Dependencies:
  PackageDependencies:
  - PackageIdentifier: Microsoft.VCRedist.2015+.x64
ReleaseDate: 2026-11-07
Installers:
- Architecture: x64
  InstallerType: zip
  InstallerUrl: {_RELEASE}/contour-0.9.0.9500-win64.zip
  InstallerSha256: 5D18A3C2DD34E0F675871862BE317CCDE210F3CD2E7A07CC51B82C4F2FBA9CCA
- InstallerLocale: en-US
  Architecture: x64
  InstallerType: wix
  Scope: user
  InstallerUrl: {_RELEASE}/contour-0.9.0.9500-win64-user.msi
  InstallerSha256: 272D2EE6AA908A0B48BFBDE629F3D5B014C5900675D2C270CF2AC46F0521086D
  InstallerSwitches:
    InstallLocation: INSTALL_ROOT="<INSTALLPATH>"
  ProductCode: '{{38815CD8-23C0-4789-8D2F-14A3AE7DBD31}}'
  AppsAndFeaturesEntries:
  - Publisher: Contour Terminal
    DisplayVersion: 0.9.0
    ProductCode: '{{38815CD8-23C0-4789-8D2F-14A3AE7DBD31}}'
    UpgradeCode: '{{AFF353C7-C33A-474C-B9C7-EDD160677BC9}}'
  InstallationMetadata:
    DefaultInstallLocation: '%LocalAppData%\\Programs\\Contour'
- InstallerLocale: en-US
  Architecture: x64
  InstallerType: wix
  Scope: machine
  InstallerUrl: {_RELEASE}/contour-0.9.0.9500-win64.msi
  InstallerSha256: 390386ED1D33E13853A5014F6C72B8107426D8C93FD1F4C09FF4132FCB2610EB
  InstallerSwitches:
    InstallLocation: INSTALL_ROOT="<INSTALLPATH>"
  ProductCode: '{{5BBA1157-5572-4F40-9597-7166644F2DDF}}'
  AppsAndFeaturesEntries:
  - Publisher: Contour Terminal
    DisplayVersion: 0.9.0
    ProductCode: '{{5BBA1157-5572-4F40-9597-7166644F2DDF}}'
    UpgradeCode: '{{0E736497-2B72-4117-95E9-54EC6D000000}}'
  InstallationMetadata:
    DefaultInstallLocation: '%ProgramFiles%\\Contour'
ManifestType: installer
ManifestVersion: 1.12.0

# Created with komac v2.17.1
# yaml-language-server: $schema=https://aka.ms/winget-manifest.defaultLocale.1.12.0.schema.json

PackageIdentifier: contour-terminal.Contour
PackageVersion: 0.9.0.9500
PackageLocale: en-US
Publisher: Contour Terminal Emulator
ManifestType: defaultLocale
ManifestVersion: 1.12.0

# Created with komac v2.17.1
# yaml-language-server: $schema=https://aka.ms/winget-manifest.version.1.12.0.schema.json

PackageIdentifier: contour-terminal.Contour
PackageVersion: 0.9.0.9500
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.12.0
"""


def _assets(names: Sequence[str] = _NAMES) -> list[tuple[str, str]]:
    return [(name, f"{_RELEASE}/{name}") for name in names]


def _plan() -> Plan:
    plan, problems = make_plan("v0.9.0", False, _assets(), "https://example/release")
    assert plan is not None, problems
    return plan


def _finished_manifest() -> dict[str, Any]:
    """The render, finished the way `finalize` does, as the dict PyYAML would load. Built without PyYAML."""
    def msi(scope: str, name: str, product: str, code: str) -> dict[str, Any]:
        return {
            "InstallerLocale": LOCALE, "Architecture": "x64", "InstallerType": "wix", "Scope": scope,
            "InstallerUrl": f"{_RELEASE}/{name}", "ProductCode": product,
            "AppsAndFeaturesEntries": [{"Publisher": "Contour Terminal", "ProductCode": product, "UpgradeCode": code}],
        }

    return {
        "PackageIdentifier": IDENTIFIER, "PackageVersion": "0.9.0.9500", "ManifestType": "installer",
        "Installers": [
            {"Architecture": "x64", "InstallerType": "zip", "InstallerLocale": LOCALE, "NestedInstallerType": "portable",
             "NestedInstallerFiles": [{"RelativeFilePath": _NESTED}], "InstallerUrl": f"{_RELEASE}/contour-0.9.0.9500-win64.zip"},
            msi("user", "contour-0.9.0.9500-win64-user.msi", "{38815CD8-23C0-4789-8D2F-14A3AE7DBD31}", "{AFF353C7-C33A-474C-B9C7-EDD160677BC9}"),
            msi("machine", "contour-0.9.0.9500-win64.msi", "{5BBA1157-5572-4F40-9597-7166644F2DDF}", "{0E736497-2B72-4117-95E9-54EC6D000000}"),
        ],
    }


def self_test() -> int:
    """Runs the inline cases; returns the process exit status."""
    cases = 0
    failures: list[str] = []

    def expect(what: str, actual: object, expected: object) -> None:
        nonlocal cases
        cases += 1
        if actual != expected:
            failures.append(f"{what}: expected {expected!r}, got {actual!r}")

    def has(what: str, problems: Sequence[str], needle: str) -> None:
        nonlocal cases
        cases += 1
        if not any(needle in problem for problem in problems):
            failures.append(f"{what}: expected a finding with '{needle}', got {list(problems)}")

    # make_plan
    plan, problems = make_plan("v0.9.0", False, _assets(), "https://example/release")
    expect("plan: good release", problems, [])
    expect("plan: version", plan.version if plan else None, "0.9.0.9500")
    expect("plan: urls", len(plan.installers) if plan else None, 3)
    for label, drop in (("per-user MSI", 1), ("portable ZIP", 2), ("per-machine MSI", 0)):
        _, problems = make_plan("v0.9.0", False, _assets(_NAMES[:drop] + _NAMES[drop + 1:]), "u")
        has(f"plan: no {label}", problems, f"no asset for the {label}")
    _, problems = make_plan("v0.9.1", False, _assets(), "u")
    has("plan: tag mismatch", problems, "tag v0.9.1 does not match the assets' version 0.9.0.9500")
    _, problems = make_plan("v0.9.0", False, _assets(["contour-0.9.0.9500-win64.msi", "contour-0.9.0.9501-win64-user.msi", "contour-0.9.0.9500-win64.zip"]), "u")
    has("plan: two versions", problems, "assets carry different versions")
    has("plan: two versions name the files", problems, "contour-0.9.0.9501-win64-user.msi (0.9.0.9501)")
    _, problems = make_plan("v0.9.0", False, _assets(list(_NAMES) + ["contour-0.9.0.9501-win64.msi"]), "u")
    has("plan: two per-machine MSIs", problems, "2 assets match the per-machine MSI: contour-0.9.0.9500-win64.msi, contour-0.9.0.9501-win64.msi")
    _, problems = make_plan("v0.9.0", True, _assets(), "u")
    has("plan: draft", problems, "release v0.9.0 is not published")
    plan4, problems = make_plan("v0.8.0.9335", False, _assets(["contour-0.8.0.9335-win64.msi", "contour-0.8.0.9335-win64-user.msi", "contour-0.8.0.9335-win64.zip"]), "u")
    expect("plan: a four-part tag", problems, [])

    # the render, split and finished
    documents = split_documents(_RENDER)
    expect("split: the three manifests", sorted(documents), ["defaultLocale", "installer", "version"])
    expect("split: file names", manifest_file_names(documents), {
        "version": "contour-terminal.Contour.yaml", "installer": "contour-terminal.Contour.installer.yaml",
        "defaultLocale": "contour-terminal.Contour.locale.en-US.yaml"})
    expect("split: CRLF", sorted(split_documents(_RENDER.replace("\n", "\r\n"))), ["defaultLocale", "installer", "version"])
    expect("split: the header stays", documents["installer"].startswith("# Created with komac v2.17.1\n"), True)
    plan = _plan()
    finished = finish_installer_manifest(documents["installer"], plan.installers["portable ZIP"], _NESTED)
    expect("finish: no Dependencies", "Dependencies" in finished, False)
    expect("finish: the nested installer", "  NestedInstallerType: portable\n  NestedInstallerFiles:\n  - RelativeFilePath: " + _NESTED + "\n" in finished, True)
    expect("finish: the ZIP's locale", finished.count("InstallerLocale: en-US"), 3)
    expect("finish: the MSIs untouched", "  Scope: machine\n" in finished and "ManifestType: installer" in finished, True)
    expect("finish: once is enough", finish_installer_manifest(finished, plan.installers["portable ZIP"], _NESTED), finished)
    try:
        finish_installer_manifest(documents["installer"], f"{_RELEASE}/other.zip", _NESTED)
        failures.append("finish: an unknown ZIP was accepted")
    except ValueError:
        pass
    cases += 1

    # the ZIP's nested file and hash
    with tempfile.TemporaryDirectory() as directory:
        archive = Path(directory) / "z.zip"
        with zipfile.ZipFile(archive, "w") as handle:
            handle.writestr("Contour-0.9.0.9500-win64/bin/contour.exe", "x")
            handle.writestr("Contour-0.9.0.9500-win64/bin/other.exe", "y")
        expect("zip: the nested file", find_nested_file(archive, "bin/contour.exe"), _NESTED)
        with zipfile.ZipFile(archive, "a") as handle:
            handle.writestr("Contour-0.9.0.9500-win64/tools/bin/contour.exe", "z")
        try:
            find_nested_file(archive, "bin/contour.exe")
            failures.append("zip: two candidates were accepted")
        except ValueError:
            pass
        cases += 1
        expect("zip: sha-256", file_sha256(archive), hashlib.sha256(archive.read_bytes()).hexdigest().upper())
        printed = Path(directory) / "komac.log"
        printed.write_text(_RENDER, encoding="utf-8")
        expect("source: the installer document", installer_manifest_text(printed).startswith("# Created with komac"), True)
        folder = Path(directory) / "manifests"
        folder.mkdir()
        (folder / "x.installer.yaml").write_text("a: 1\n", encoding="utf-8")
        expect("source: a directory", installer_manifest_text(folder), "a: 1\n")

    # check_manifest
    expect("manifest: good", check_manifest(plan, _finished_manifest()), [])

    def broken(change: Any) -> list[str]:
        manifest = _finished_manifest()
        change(manifest)
        return check_manifest(plan, manifest)

    has("manifest: per-user without Scope", broken(lambda m: m["Installers"][1].pop("Scope")), "per-user MSI: Scope is missing, expected user")
    has("manifest: wrong UpgradeCode", broken(lambda m: m["Installers"][2]["AppsAndFeaturesEntries"][0].update(UpgradeCode="{0E736497-2B72-4117-95E9-54EC6D000001}")), "per-machine MSI: UpgradeCode")
    has("manifest: no entries", broken(lambda m: m["Installers"][1].pop("AppsAndFeaturesEntries")), "per-user MSI: AppsAndFeaturesEntries")
    has("manifest: shared ProductCode", broken(lambda m: m["Installers"][1].update(ProductCode=m["Installers"][2]["ProductCode"])), "ProductCode")
    has("manifest: short version", broken(lambda m: m.update(PackageVersion="0.9.0")), "expected 0.9.0.9500")
    has("manifest: foreign url", broken(lambda m: m["Installers"][1].update(InstallerUrl="https://elsewhere/x.msi")), "https://elsewhere/x.msi is not in the plan")
    has("manifest: missing url", broken(lambda m: m["Installers"][1].update(InstallerUrl="https://elsewhere/x.msi")), "per-user MSI: no installer for")
    has("manifest: an installer twice", broken(lambda m: m["Installers"].append(dict(m["Installers"][2]))), "is listed 2 times")
    has("manifest: Dependencies", broken(lambda m: m.update(Dependencies={"PackageDependencies": []})), "Dependencies is present")
    has("manifest: Dependencies of an installer", broken(lambda m: m["Installers"][1].update(Dependencies={})), "Dependencies is present")
    has("manifest: ZIP without locale", broken(lambda m: m["Installers"][0].pop("InstallerLocale")), "portable ZIP: InstallerLocale is None")
    has("manifest: an MSI in another locale", broken(lambda m: m["Installers"][1].update(InstallerLocale="de-DE")), "per-user MSI: InstallerLocale is de-DE")
    has("manifest: ZIP without nested type", broken(lambda m: m["Installers"][0].pop("NestedInstallerType")), "NestedInstallerType is None")
    has("manifest: ZIP nested elsewhere", broken(lambda m: m["Installers"][0].update(NestedInstallerFiles=[{"RelativeFilePath": "x/bin/bench-headless.exe"}])), "no nested file ending in bin/contour.exe")
    has("manifest: ZIP with a scope", broken(lambda m: m["Installers"][0].update(Scope="user")), "portable ZIP: Scope is user, expected none")
    has("manifest: ZIP typed wix", broken(lambda m: m["Installers"][0].update(InstallerType="wix")), "portable ZIP: InstallerType is wix")

    def hoisted(manifest: dict[str, Any]) -> None:
        for installer in manifest["Installers"][1:]:
            installer.pop("InstallerType")
        manifest["InstallerType"] = "wix"

    expect("manifest: a type at the root and the ZIP overriding it", broken(hoisted), [])

    # winget's choice, whatever the order (the preference for the user scope is strong), and without the ZIP's help
    for order in ((0, 1, 2), (2, 1, 0), (1, 2, 0), (2, 0, 1)):
        manifest = _finished_manifest()
        manifest["Installers"] = [manifest["Installers"][i] for i in order]
        expect(f"select {order}: no manifest finding", check_manifest(plan, manifest), [])
    manifest = _finished_manifest()
    expect("select: --scope machine picks the machine MSI", select_installer(manifest, "machine") is manifest["Installers"][2], True)
    expect("select: --scope user picks the user MSI", select_installer(manifest, "user") is manifest["Installers"][1], True)
    expect("select: a plain install picks the user MSI", select_installer(manifest, None) is manifest["Installers"][1], True)
    manifest["Installers"].pop(1)
    expect("select: without the user MSI, a plain install takes the machine MSI over the ZIP", select_installer(manifest, None) is manifest["Installers"][1], True)
    expect("select: nothing for an empty manifest", select_installer({"Installers": []}, None), None)

    # undeclared_upgrade_codes
    code = INSTALLERS[0].upgrade_code or ""
    expect("codes: lower case with braces", undeclared_upgrade_codes([code], ["x {" + code.lower() + "} y"]), [])
    expect("codes: absent", undeclared_upgrade_codes([code], ["nothing here"]), [code])

    # pull requests
    pulls = [
        {"url": "https://github.com/microsoft/winget-pkgs/pull/1", "title": "New version: contour-terminal.Contour version 0.9.0.9500", "state": "CLOSED"},
        {"url": "https://github.com/microsoft/winget-pkgs/pull/2", "title": "New version: contour-terminal.Contour version 0.9.0.95", "state": "OPEN"},
        {"url": "https://github.com/microsoft/winget-pkgs/pull/3", "title": "New version: contour-terminal.Contour version 10.9.0.9500", "state": "OPEN"},
    ]
    expect("pull request: closed, near misses", pick_pull_request(pulls, "0.9.0.9500"), None)
    pulls.append({"url": "https://github.com/microsoft/winget-pkgs/pull/4", "title": "New version: contour-terminal.Contour version 0.9.0.9500", "state": "MERGED"})
    expect("pull request: merged", pick_pull_request(pulls, "0.9.0.9500"), "https://github.com/microsoft/winget-pkgs/pull/4")
    pulls[3]["state"] = "OPEN"
    expect("pull request: open", pick_pull_request(pulls, "0.9.0.9500"), "https://github.com/microsoft/winget-pkgs/pull/4")
    expect("pull request: in Komac's output", pull_request_in_output("Successfully created a pull request: https://github.com/microsoft/winget-pkgs/pull/123456 done"), "https://github.com/microsoft/winget-pkgs/pull/123456")
    expect("pull request: none in the output", pull_request_in_output("nothing"), None)

    if failures:
        for failure in failures:
            print(f"winget-release self-test: {failure}", file=sys.stderr)
        return 1
    print(f"winget-release self-test: {cases} cases OK.")
    return 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="run the inline cases")
    commands = parser.add_subparsers(dest="command")
    plan_parser = commands.add_parser("plan", help="plan the submission of a published release")
    plan_parser.add_argument("--tag", required=True)
    plan_parser.add_argument("--out", required=True, type=Path)
    finalize_parser = commands.add_parser("finalize", help="finish Komac's printed manifests and write them")
    finalize_parser.add_argument("--plan", required=True, type=Path)
    finalize_parser.add_argument("--render", required=True, type=Path, help="the text Komac printed")
    finalize_parser.add_argument("--zip", required=True, type=Path, help="the release's portable ZIP")
    finalize_parser.add_argument("--out", required=True, type=Path, help="the folder to write the manifests to")
    check_parser = commands.add_parser("check", help="check the installer manifest")
    check_parser.add_argument("--plan", required=True, type=Path)
    check_parser.add_argument("source", type=Path, help="a manifest directory, or the text Komac printed")
    existing = commands.add_parser("existing-pr", help="the open or merged pull request for a version")
    existing.add_argument("--version", required=True)
    submitted = commands.add_parser("submitted-pr", help="the pull request Komac opened")
    submitted.add_argument("--log", required=True, type=Path)
    submitted.add_argument("--version", required=True)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "plan":
        return run_plan(args.tag, args.out)
    if args.command == "finalize":
        return run_finalize(args.plan, args.render, args.zip, args.out)
    if args.command == "check":
        return run_check(args.plan, args.source)
    if args.command == "existing-pr":
        write_outputs({"url": search_pull_requests(args.version) or ""})
        return 0
    if args.command == "submitted-pr":
        url = pull_request_in_output(args.log.read_text(encoding="utf-8", errors="replace")) or search_pull_requests(args.version)
        if not url:
            print(f"winget-release: no pull request for {args.version} on {WINGET_PKGS}", file=sys.stderr)
            return 1
        print(url)
        return 0
    return run_sources()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
