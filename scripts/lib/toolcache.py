"""Acquires a pinned, prebuilt lint tool, on any platform this project supports.

Both lint gates -- `check-spelling.py` (typos) and `check-workflows.py` (actionlint) -- need one
specific version of one statically linked binary, without asking the developer to install anything.
The steps are identical for both, so they live here once rather than being written twice.

Two rules the callers depend on:

*   **The pin wins.** A copy on PATH is used only if it reports exactly the pinned version. A
    developer's stray install must not be able to disagree with CI, because the point of these
    gates is that a local run and a CI run are the same command.

*   **A missing tool skips; CI turns the skip into a failure.** See `unavailable`.
"""

from __future__ import annotations

import io
import os
import platform
import shutil
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

#: The repository root, derived from this file's own location: scripts/lib/toolcache.py.
REPO_ROOT = Path(__file__).resolve().parents[2]

# Long enough to survive a slow link on a cold cache, short enough that an unreachable host fails
# the check rather than hanging until ctest's own timeout fires.
_DOWNLOAD_TIMEOUT_SECONDS = 60

# GitHub serves release assets without one, but some corporate proxies reject a request that has no
# User-Agent at all.
_USER_AGENT = "contour-lint-gate"

# platform.machine() answers with whatever the host calls its architecture: `AMD64` on Windows,
# `x86_64` on Linux, `arm64` on macOS, `aarch64` on Linux arm. Fold those into one spelling so each
# tool's asset table has a single key per architecture.
_ARCH_ALIASES = {
    "amd64": "x86_64",
    "x64": "x86_64",
    "x86_64": "x86_64",
    "aarch64": "aarch64",
    "arm64": "aarch64",
}


class FetchError(Exception):
    """A release archive could not be downloaded or read.

    Exists so that `resolve` can handle every failure from the fetch path without importing the
    modules that raise them -- see the deferred imports in `_download` and `_extract_binary`.
    """


@dataclass(frozen=True)
class ToolSpec:
    """Everything that distinguishes one downloadable lint tool from another.

    Adding a third tool is meant to be adding one of these, not editing any logic below.
    """

    #: Executable name, without any platform suffix -- `typos`, `actionlint`.
    name: str
    #: The pinned version, exactly as the tool's own `--version` reports it.
    version: str
    #: Release URL, formatted with the selected `asset`.
    url_template: str
    #: (system, architecture) -> release asset filename. `system` is `platform.system()`;
    #: `architecture` is normalized per `_ARCH_ALIASES`. A platform absent from this table has no
    #: published binary, and the gate skips there.
    assets: dict[tuple[str, str], str]
    #: Environment variable that turns the "tool unavailable" skip into a hard failure. CI sets it.
    require_env: str
    #: Environment variable overriding where the downloaded binary is cached.
    cache_env: str


def _executable_name(spec: ToolSpec) -> str:
    return spec.name + ".exe" if platform.system() == "Windows" else spec.name


def _host_key() -> tuple[str, str]:
    machine = _ARCH_ALIASES.get(platform.machine().lower(), platform.machine())
    return (platform.system(), machine)


def _installed_version(executable: Path | str) -> str | None:
    """Reports the version an executable claims, or None if it cannot be asked.

    The two tools disagree on format -- typos prints `typos-cli 1.48.0`, actionlint prints a bare
    `1.7.12` -- so take the last whitespace-separated token of the first line, which parses both.
    A tool whose format defeats this is caught at install time rather than silently re-downloading
    on every run; see `resolve`.
    """
    try:
        completed = subprocess.run(
            [str(executable), "--version"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    lines = completed.stdout.strip().splitlines()
    if not lines:
        return None
    fields = lines[0].split()
    return fields[-1] if fields else None


def _extract_binary(archive: bytes, asset: str, member_name: str) -> bytes:
    """Pulls one named binary out of a release archive held in memory.

    Members are matched on their *base* name because the two tools disagree on the rest of it:
    typos stores its binary as `./typos`, actionlint as plain `actionlint`.

    The archive is never written to disk. That keeps a cold run from leaving a temporary file
    behind, and sidesteps Windows' refusal to unlink a file that is still open.
    """
    # Deferred: these pull in ssl, http.client and the compression codecs, which the warm path --
    # every `ctest -L lint` after the first -- never needs. Importing them at module scope cost
    # ~25ms in each of the three lint processes.
    import tarfile
    import zipfile

    try:
        if asset.endswith(".zip"):
            with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
                for entry in bundle.infolist():
                    if not entry.is_dir() and Path(entry.filename).name == member_name:
                        return bundle.read(entry)
            return b""

        # `r:*` rather than `r:gz`: it auto-detects the compression, so a future tool shipping
        # .tar.xz or .tar.bz2 needs no change here. Iterated forward rather than via getmembers(),
        # which builds its index by reading to EOF and then has to re-inflate from byte zero.
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:*") as bundle:
            for entry in bundle:
                if entry.isfile() and Path(entry.name).name == member_name:
                    extracted = bundle.extractfile(entry)
                    if extracted is not None:
                        with extracted:
                            return extracted.read()
            return b""
    except (OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        raise FetchError(str(error)) from error


def _download(url: str) -> bytes:
    import urllib.error
    import urllib.request

    request = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=_DOWNLOAD_TIMEOUT_SECONDS) as response:
            return response.read()
    except (OSError, urllib.error.URLError) as error:
        raise FetchError(str(error)) from error


def _install(binary: bytes, destination: Path) -> None:
    """Writes the binary into the cache, atomically and executable."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    staged = destination.with_name(destination.name + ".partial")
    staged.write_bytes(binary)
    staged.chmod(staged.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    # os.replace, not rename: only replace() overwrites an existing file on Windows.
    os.replace(staged, destination)


def unavailable(spec: ToolSpec, reason: str) -> NoReturn:
    """Reports that the tool could not be obtained, then skips or fails, and does not return.

    CI wants a red build -- a gate that silently passes because it could not download its tool is
    worse than no gate. A developer offline in a fresh clone wants to get on with their day.
    """
    if os.environ.get(spec.require_env) == "1":
        print(
            f"{spec.name} v{spec.version} is required but could not be obtained: {reason}",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"{spec.name} not available ({reason}); skipping check.", file=sys.stderr)
    sys.exit(0)


def resolve(spec: ToolSpec) -> Path:
    """Returns a path to the tool at exactly the pinned version, downloading it if need be.

    Does not return if the tool cannot be obtained: see `unavailable`.
    """
    executable = _executable_name(spec)
    cache_dir = Path(os.environ.get(spec.cache_env) or REPO_ROOT / "out" / ".tools")
    cached = cache_dir / executable

    on_path = shutil.which(spec.name)
    if on_path is not None and _installed_version(on_path) == spec.version:
        return Path(on_path)
    if cached.exists() and _installed_version(cached) == spec.version:
        return cached

    host = _host_key()
    asset = spec.assets.get(host)
    if asset is None:
        unavailable(spec, f"no release asset for {host[0]}/{host[1]}")

    url = spec.url_template.format(asset=asset)
    try:
        binary = _extract_binary(_download(url), asset, executable)
    except FetchError as error:
        unavailable(spec, f"download from {url} failed: {error}")

    if not binary:
        unavailable(spec, f"{asset} contains no {executable}")

    _install(binary, cached)

    # A freshly installed binary that does not report the pinned version means the version parse
    # above is wrong for this tool. Left unchecked that is invisible and permanent: every run would
    # reject the cache and download again. Fail loudly instead, whatever `require_env` says --
    # this is a defect in the spec, not a network problem a developer can wait out.
    reported = _installed_version(cached)
    if reported != spec.version:
        print(
            f"{spec.name}: installed {asset} reports version {reported!r}, expected "
            f"{spec.version!r}. The version pin cannot be verified; fix the ToolSpec.",
            file=sys.stderr,
        )
        sys.exit(1)
    return cached


def run(tool: Path, arguments: list[str]) -> int:
    """Runs the tool from the repository root and returns its exit code."""
    return subprocess.run([str(tool), *arguments], cwd=REPO_ROOT, check=False).returncode
