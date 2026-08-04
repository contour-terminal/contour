#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify, code-sign and notarize a macOS .app bundle or .dmg image.

The four subcommands are what stands between a freshly ``macdeployqt``-ed bundle and
an artifact macOS will actually open:

``verify``
    Walk every Mach-O in the bundle and resolve each of its dynamic library
    references. A reference that points outside the bundle, or that cannot be
    resolved at all, means the app will fail to launch on a machine that does not
    happen to have the build host's libraries installed. Exits non-zero on any
    finding, so a broken bundle breaks the build instead of shipping. Also reports
    the oldest macOS the bundle can run on.

``prune``
    Delete bundled plugins from a small allowlist of categories that macdeployqt is
    known to over-deploy, when they reference libraries that exist only on the build
    machine. macdeployqt deploys every plugin in a category once any module pulls
    that category in, so a bundle picks up (say) the Mimer and PostgreSQL SQL drivers
    along with SQLite. The allowlist is deliberate: an unloadable plugin *outside* it
    is a deployment bug that should fail ``verify`` loudly, not vanish silently --
    quietly dropping, say, ``platforminputcontexts`` would cost IME input with no
    diagnostic anywhere.

``sign``
    Sign the bundle strictly inside-out with the hardened runtime enabled, then
    assert the properties notarization requires. ``codesign --deep`` is deliberately
    not used: Apple documents it as unsuitable for distribution signing because it
    applies the top-level entitlements to nested code and does not guarantee the
    nested-first order a valid seal requires.

``notarize``
    Submit to Apple's notary service, wait for the verdict, and staple the resulting
    ticket. Without a stapled ticket Gatekeeper blocks the artifact regardless of how
    correct the signature is.

``prune`` and ``verify`` both exist to compensate for macdeployqt: it over-deploys
plugin categories, and it reports unresolvable frameworks on stderr while still
exiting 0. Qt's own replacement, ``qt_generate_deploy_qml_app_script()``, resolves
the QML import closure through ``qmlimportscanner`` and dylibs through CMake's
``file(GET_RUNTIME_DEPENDENCIES)``, which would retire ``prune`` and reduce ``verify``
to a belt-and-braces check. Migrating to it would retire most of this file.

See docs/internals/macos-code-signing.md for how these fit into a release.
"""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

# Prefixes owned by the OS. References into these are expected and are left alone;
# anything else outside the bundle is a portability bug.
SYSTEM_PREFIXES = ('/usr/lib/', '/System/', '/Library/Apple/')

# Plugin categories `prune` is allowed to delete from. Everything Contour needs to
# start or to render is outside this set, so a broken plugin elsewhere surfaces as a
# verify failure. Add a row when macdeployqt is found over-deploying another category.
PRUNABLE_PLUGIN_CATEGORIES = frozenset((
    # Deployed wholesale because QtQml pulls in QtQmlLocalStorage -> QtSql, even though
    # nothing here uses SQL. The Mimer, ODBC and PostgreSQL drivers link against
    # libraries that are not on an end user's machine (and often not on the builder's).
    'sqldrivers',
))

# otool accepts many files per invocation, which turns ~135 process spawns into one.
# Chunked so a bundle with very long paths cannot overflow the argument list.
_OTOOL_BATCH = 500


class Failure(Exception):
    """A user-facing error: printed without a traceback, exits non-zero."""


def log(message: str) -> None:
    print(f"macos-bundle: {message}", flush=True)


def run(argv: list[str]) -> str:
    """Run a command and return its stdout, raising Failure if it fails."""
    result = subprocess.run(argv, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise Failure(f"{argv[0]} failed ({result.returncode}): {detail}")
    return result.stdout


# {{{ Mach-O inspection

@dataclass(frozen=True)
class MachO:
    """The load-command facts about one Mach-O file that deployment depends on.

    Universal binaries -- which official Qt's plugins are -- carry one load-command
    section per architecture; the fields below merge them, since a reference that is
    unresolvable for one slice is a bug for the whole file.
    """

    path: Path
    install_id: str | None
    dependencies: tuple[str, ...]
    rpaths: tuple[str, ...]
    minimum_os: tuple[int, ...] | None


# Load commands that name another library the loader has to find.
_DEPENDENCY_COMMANDS = frozenset((
    'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB',
    'LC_LOAD_UPWARD_DYLIB', 'LC_LAZY_LOAD_DYLIB',
))


def is_macho(path: Path) -> bool:
    """Whether the file starts with a Mach-O (or universal binary) magic number."""
    try:
        with path.open('rb') as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (
        b'\xcf\xfa\xed\xfe',  # MH_MAGIC_64, little endian
        b'\xfe\xed\xfa\xcf',  # MH_CIGAM_64
        b'\xca\xfe\xba\xbe',  # FAT_MAGIC
        b'\xbe\xba\xfe\xca',  # FAT_CIGAM
    )


def find_binaries(root: Path) -> list[Path]:
    """Every Mach-O file under root, symlinks excluded (they alias a real entry).

    Every candidate is probed by reading its magic number rather than filtered on a
    suffix list: a suffix list that accidentally matches a real Mach-O would drop it
    from signing, and unsigned nested code is a notarization rejection.
    """
    binaries = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for filename in filenames:
            path = Path(dirpath) / filename
            if not path.is_symlink() and is_macho(path):
                binaries.append(path)
    return sorted(binaries)


def inspect_all(paths: list[Path]) -> dict[Path, MachO]:
    """Read install name, dependencies, rpaths and minimum macOS for many binaries."""
    images: dict[Path, MachO] = {}
    for start in range(0, len(paths), _OTOOL_BATCH):
        chunk = paths[start:start + _OTOOL_BATCH]
        output = run(['otool', '-l', *(str(path) for path in chunk)])
        images.update(_parse_load_commands(output, chunk))

    missing = [path for path in paths if path not in images]
    if missing:
        raise Failure(f"otool produced no load commands for: {missing[0]} "
                      f"(and {len(missing) - 1} more)")
    return images


def inspect(path: Path) -> MachO:
    """Read one binary's load-command facts."""
    return inspect_all([path])[path]


def _parse_load_commands(output: str, paths: list[Path]) -> dict[Path, MachO]:
    """Demultiplex `otool -l` output covering several files.

    Each file contributes a header line at column 0 ending in a colon -- either
    `<path>:` for a thin binary, or one `<path> (architecture <arch>):` per slice of a
    universal one. Fields from every slice of a file are merged into a single MachO.
    """
    wanted = {str(path): path for path in paths}
    accumulators: dict[Path, _Accumulator] = {}
    current: _Accumulator | None = None
    command: str | None = None

    for raw in output.splitlines():
        if raw and not raw[0].isspace() and raw.endswith(':'):
            name = raw[:-1].split(' (architecture ')[0]
            path = wanted.get(name)
            if path is not None:
                current = accumulators.setdefault(path, _Accumulator(path))
                command = None
                continue

        if current is None:
            continue

        line = raw.strip()
        if line.startswith('cmd '):
            command = line[len('cmd '):]
        elif command is not None:
            current.consume(command, line)

    return {path: acc.finish() for path, acc in accumulators.items()}


class _Accumulator:
    """Collects the load-command fields of one file across its architecture slices."""

    def __init__(self, path: Path) -> None:
        self._path = path
        self._install_id: str | None = None
        self._dependencies: list[str] = []
        self._rpaths: list[str] = []
        self._minimum_os: tuple[int, ...] | None = None

    def consume(self, command: str, line: str) -> None:
        # `name <path> (offset N)` for the dylib commands, `path <path> (offset N)` for
        # LC_RPATH, `minos <version>` for LC_BUILD_VERSION.
        if command in _DEPENDENCY_COMMANDS and line.startswith('name '):
            _append_unique(self._dependencies, _strip_offset(line[len('name '):]))
        elif command == 'LC_ID_DYLIB' and line.startswith('name '):
            self._install_id = _strip_offset(line[len('name '):])
        elif command == 'LC_RPATH' and line.startswith('path '):
            _append_unique(self._rpaths, _strip_offset(line[len('path '):]))
        elif command == 'LC_BUILD_VERSION' and line.startswith('minos '):
            version = _parse_version(line[len('minos '):].strip())
            if version is not None and (self._minimum_os is None
                                        or version > self._minimum_os):
                self._minimum_os = version

    def finish(self) -> MachO:
        return MachO(self._path, self._install_id, tuple(self._dependencies),
                     tuple(self._rpaths), self._minimum_os)


def _append_unique(values: list[str], value: str) -> None:
    if value not in values:
        values.append(value)


def _strip_offset(value: str) -> str:
    return value.split(' (offset ')[0].strip()


def _parse_version(text: str) -> tuple[int, ...] | None:
    try:
        return tuple(int(part) for part in text.split('.'))
    except ValueError:
        return None

# }}}
# {{{ dyld reference resolution


def is_system_path(path: str) -> bool:
    return path.startswith(SYSTEM_PREFIXES)


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def resolve(reference: str, image: MachO, executable: MachO) -> Path | None:
    """Resolve one dylib reference the way dyld would, or None if nothing matches."""
    if reference.startswith('@rpath/'):
        tail = reference[len('@rpath/'):]
        # dyld expands @rpath against the LC_RPATHs of the referring image *and* those
        # of the main executable. Qt relies on the second half: its plugins keep the
        # build-tree rpath `@loader_path/../../lib`, which does not exist in a bundle,
        # and are found only through the app's own `@executable_path/../Frameworks`.
        # A verifier that checked the plugin's own rpaths alone would report every Qt
        # plugin as broken.
        for rpath in (*image.rpaths, *executable.rpaths):
            candidate = expand(rpath, image.path, executable.path)
            if candidate is not None and (candidate / tail).exists():
                return candidate / tail
        return None
    expanded = expand(reference, image.path, executable.path)
    if expanded is None:
        return None
    return expanded if expanded.exists() else None


def expand(path: str, binary: Path, executable: Path) -> Path | None:
    """Substitute dyld's @loader_path / @executable_path placeholders."""
    if path.startswith('@loader_path'):
        return (binary.parent / path[len('@loader_path'):].lstrip('/')).resolve()
    if path.startswith('@executable_path'):
        return (executable.parent / path[len('@executable_path'):].lstrip('/')).resolve()
    if path.startswith('@'):
        return None  # @rpath is handled by the caller; nothing else is expected.
    return Path(path)


def unsatisfied(image: MachO, executable: MachO, bundle: Path) -> list[tuple[str, str]]:
    """The references of one image that would not resolve, each with its reason.

    The single definition of "this cannot load", shared by `verify` (which formats the
    reasons) and `prune` (which only asks whether the list is empty). Keeping them on
    one predicate is what stops the two from drifting into disagreement, where prune
    would delete a plugin that verify considered fine, or vice versa.
    """
    findings = []
    identifier = image.install_id
    if identifier and not identifier.startswith('@') and not is_system_path(identifier):
        findings.append((identifier, 'install name points outside the bundle'))

    for dep in image.dependencies:
        if is_system_path(dep):
            continue
        if not dep.startswith('@'):
            findings.append((dep, 'absolute dependency outside the bundle'))
            continue
        target = resolve(dep, image, executable)
        if target is None:
            findings.append((dep, 'unresolvable dependency'))
        elif not is_within(target, bundle):
            findings.append((dep, f'dependency resolves outside the bundle ({target})'))
    return findings


def main_executable(bundle: Path) -> Path:
    """The bundle's CFBundleExecutable, as dyld's @executable_path anchor."""
    info = bundle / 'Contents' / 'Info.plist'
    if not info.exists():
        raise Failure(f"not an app bundle (no Contents/Info.plist): {bundle}")
    with info.open('rb') as f:
        name = plistlib.load(f).get('CFBundleExecutable')
    if not name:
        raise Failure(f"Info.plist has no CFBundleExecutable: {info}")
    executable = bundle / 'Contents' / 'MacOS' / name
    if not executable.exists():
        raise Failure(f"CFBundleExecutable does not exist: {executable}")
    return executable

# }}}
# {{{ verify


def verify(bundle: Path, max_minimum_os: tuple[int, ...] | None = None) -> int:
    """Report every dylib reference that would not resolve on a user's machine."""
    executable_path = main_executable(bundle)
    paths = find_binaries(bundle)
    log(f"verify: scanning {len(paths)} Mach-O binaries in {bundle.name}")

    images = inspect_all(paths)
    executable = images[executable_path]

    findings = [f"{image.path.relative_to(bundle)}: {reason}: {reference}"
                for image in images.values()
                for reference, reason in unsatisfied(image, executable, bundle)]

    floor = report_minimum_os(bundle, images.values())
    if max_minimum_os is not None and floor is not None and floor > max_minimum_os:
        findings.append(
            f"the bundle requires macOS {_format_version(floor)}, but this build promises "
            f"support down to {_format_version(max_minimum_os)} -- users below "
            f"{_format_version(floor)} would be unable to launch it")

    if findings:
        log(f"verify: FAILED with {len(findings)} finding(s):")
        for finding in findings:
            print(f"  - {finding}", file=sys.stderr)
        print("\nThe bundle is not self-contained: it would fail to launch on any machine\n"
              "that does not happen to have the build host's libraries. This is a deployment\n"
              "problem, not a signing problem -- signing a broken bundle just produces a\n"
              "valid signature over something that cannot run.\n"
              "\n"
              "The usual cause is a Qt installed as separate per-module prefixes (Homebrew\n"
              "splits Qt across ~40 formulae), which macdeployqt cannot follow: it reports\n"
              "'Cannot resolve rpath' on stderr and then exits 0. Use the official Qt for\n"
              "packaging -- see docs/internals/macos-code-signing.md.",
              file=sys.stderr)
        return 1

    log(f"verify: OK, {bundle.name} is self-contained")
    return 0


def report_minimum_os(bundle: Path, images: Iterator[MachO]) -> tuple[int, ...] | None:
    """Print and return the oldest macOS the bundle as a whole can run on.

    Reporting this matters because nothing else surfaces it -- an app whose own binary
    targets macOS 13 still refuses to start on 13 if one bundled dylib wants 26. Whether
    the number is acceptable is the caller's decision, via --max-minimum-os.
    """
    declared = [image for image in images if image.minimum_os is not None]
    if not declared:
        return None

    highest = max(image.minimum_os for image in declared)
    culprits = sorted(image.path for image in declared if image.minimum_os == highest)
    log(f"verify: bundle requires macOS {_format_version(highest)} or newer, "
        f"set by {len(culprits)} binary/binaries, e.g.:")
    for path in culprits[:5]:
        print(f"    {path.relative_to(bundle)}")
    return highest


def _format_version(version: tuple[int, ...]) -> str:
    return '.'.join(str(part) for part in version)

# }}}
# {{{ prune


def prune(bundle: Path) -> int:
    """Remove unloadable plugins from the categories in PRUNABLE_PLUGIN_CATEGORIES."""
    plugins = bundle / 'Contents' / 'PlugIns'
    if not plugins.exists():
        log("prune: no Contents/PlugIns, nothing to do")
        return 0

    executable = inspect(main_executable(bundle))
    candidates = [path for path in find_binaries(plugins)
                  if _category_of(path, plugins) in PRUNABLE_PLUGIN_CATEGORIES]
    if not candidates:
        log("prune: no plugins in a prunable category")
        return 0

    removed = 0
    for path, image in inspect_all(candidates).items():
        broken = unsatisfied(image, executable, bundle)
        if not broken:
            continue
        reasons = ', '.join(reference for reference, _ in broken)
        log(f"prune: removing {path.relative_to(bundle)} (cannot load: {reasons})")
        path.unlink()
        removed += 1

    log(f"prune: removed {removed} unloadable plugin(s)")
    return 0


def _category_of(plugin: Path, plugins_dir: Path) -> str:
    """The plugin category directory a bundled plugin sits in, e.g. 'sqldrivers'."""
    parts = plugin.relative_to(plugins_dir).parts
    return parts[0] if len(parts) > 1 else ''

# }}}
# {{{ sign


def signing_targets(bundle: Path) -> Iterator[Path]:
    """Every item needing its own signature, in inside-out order.

    Nested code must be sealed before its container, otherwise the container's
    seal covers a signature that is later replaced.
    """
    contents = bundle / 'Contents'
    executable = main_executable(bundle)

    # Loadable code, deepest first. Qt scatters this across three directories:
    # Frameworks/ for the libraries, PlugIns/ for the platform and image plugins,
    # and Resources/qml/ for the per-QML-module plugin dylibs.
    for binary in find_binaries(contents):
        # A framework's own binary is sealed by signing the framework, below; the main
        # executable is sealed second-to-last, once everything it loads is done.
        if '.framework/' in str(binary.relative_to(contents)) or binary == executable:
            continue
        yield binary

    # Frameworks are signed as bundles, not as bare Mach-O files, so that codesign seals
    # their Info.plist and Resources too. Searched bundle-wide rather than under
    # Frameworks/ alone, so a framework nested anywhere still gets a real signature
    # instead of being silently left out of the seal.
    for framework in sorted(contents.rglob('*.framework')):
        # Versioned frameworks are signed at Versions/<v>, unversioned at the root.
        current = framework / 'Versions' / 'Current'
        yield current.resolve() if current.exists() else framework

    yield executable
    yield bundle  # The bundle itself, last: its seal covers everything above.


def sign(bundle: Path, identity: str, entitlements: Path | None) -> int:
    """Sign the bundle inside-out with the hardened runtime enabled."""
    if entitlements is not None and not entitlements.exists():
        raise Failure(f"entitlements file not found: {entitlements}")

    # Extended attributes (notably com.apple.quarantine, and the resource forks
    # some build steps leave behind) make codesign fail with "resource fork,
    # Finder information, or similar detritus not allowed".
    run(['xattr', '-cr', str(bundle)])

    targets = list(signing_targets(bundle))
    log(f"sign: signing {len(targets)} items as {identity!r}")

    for target in targets:
        argv = ['codesign', '--force', '--timestamp', '--options=runtime']
        # Only the outermost signature carries entitlements. Applying them to
        # nested code is what `--deep` gets wrong.
        if target == bundle and entitlements is not None:
            argv += ['--entitlements', str(entitlements)]
        argv += ['--sign', identity, str(target)]
        run(argv)

    # --deep is wrong for *signing* but is exactly right for verification: here it
    # means "check every nested signature too".
    run(['codesign', '--verify', '--deep', '--strict', '--verbose=2', str(bundle)])
    assert_distributable(bundle, identity)
    log(f"sign: OK, {bundle.name} signed and verified")
    return 0


def assert_distributable(bundle: Path, identity: str) -> None:
    """Check the two signature properties notarization requires but --verify ignores.

    A bundle can verify perfectly and still be rejected by Apple for lacking the
    hardened runtime, so asserting it here -- rather than only in CI -- means a local
    `cpack` is held to the same standard as a release build.
    """
    # codesign writes its description to stderr, not stdout, so this cannot go through
    # run(); --verbose is diagnostic output rather than a result.
    described = subprocess.run(
        ['codesign', '-dv', '--verbose=4', str(bundle)],
        capture_output=True, text=True, check=False,
    )
    if described.returncode != 0:
        raise Failure(f"could not read the signature of {bundle.name}: "
                      f"{described.stderr.strip()}")
    description = described.stderr + described.stdout

    if 'runtime' not in _flags_of(description):
        raise Failure("the hardened runtime is not enabled on the signed bundle; "
                      "notarization would reject it")
    if identity != '-' and 'Authority=Developer ID Application' not in description:
        raise Failure(f"expected a Developer ID Application authority after signing with "
                      f"{identity!r}, got:\n{description}")


def _flags_of(description: str) -> str:
    for line in description.splitlines():
        _, _, tail = line.partition('flags=')
        if tail:
            return tail
    return ''


def sign_file(path: Path, identity: str, entitlements: Path | None) -> int:
    """Sign a standalone file (a .dmg). No hardened runtime: it holds no code."""
    if entitlements is not None:
        raise Failure(f"--entitlements applies to an .app bundle, not to {path.name}")
    log(f"sign: signing {path.name} as {identity!r}")
    run(['codesign', '--force', '--timestamp', '--sign', identity, str(path)])
    run(['codesign', '--verify', '--strict', '--verbose=2', str(path)])
    log(f"sign: OK, {path.name} signed")
    return 0

# }}}
# {{{ notarize


def notarize(artifact: Path, profile: str) -> int:
    """Submit to Apple's notary service, wait for the verdict, then staple the ticket."""
    with tempfile.TemporaryDirectory() as tmp:
        if artifact.is_dir():
            # notarytool takes .zip, .dmg or .pkg. ditto is the only archiver that
            # preserves the signature's symlinks and extended attributes intact.
            payload = Path(tmp) / f"{artifact.name}.zip"
            log(f"notarize: archiving {artifact.name}")
            run(['ditto', '-c', '-k', '--keepParent', str(artifact), str(payload)])
        else:
            payload = artifact

        log(f"notarize: submitting {payload.name} (this waits for Apple, typically 1-5 min)")
        output = run([
            'xcrun', 'notarytool', 'submit', str(payload),
            '--keychain-profile', profile, '--wait', '--output-format', 'json',
        ])

    status, submission_id = parse_submission(output)
    if status != 'Accepted':
        if submission_id:
            log(f"notarize: rejected, fetching log for submission {submission_id}")
            print(subprocess.run(
                ['xcrun', 'notarytool', 'log', submission_id, '--keychain-profile', profile],
                capture_output=True, text=True, check=False,
            ).stdout, file=sys.stderr)
        raise Failure(f"notarization was not accepted (status: {status})")

    log(f"notarize: accepted (submission {submission_id})")
    log(f"notarize: stapling ticket to {artifact.name}")
    run(['xcrun', 'stapler', 'staple', str(artifact)])
    run(['xcrun', 'stapler', 'validate', str(artifact)])
    log(f"notarize: OK, {artifact.name} is notarized and stapled")
    return 0


def parse_submission(output: str) -> tuple[str, str]:
    try:
        payload = json.loads(output)
    except json.JSONDecodeError as error:
        raise Failure(f"could not parse notarytool output: {error}\n{output}") from error
    return payload.get('status', 'Unknown'), payload.get('id', '')

# }}}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog='macos-bundle.py', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)

    p_verify = sub.add_parser('verify', help='check that the bundle is self-contained')
    p_verify.add_argument('bundle', type=Path)
    p_verify.add_argument('--max-minimum-os', default=None, metavar='VERSION',
                          help='fail if the bundle needs a macOS newer than this '
                               '(e.g. 15.0), i.e. the oldest release this build promises')

    p_prune = sub.add_parser('prune', help='delete bundled plugins that cannot load')
    p_prune.add_argument('bundle', type=Path)

    p_sign = sub.add_parser('sign', help='sign a bundle inside-out, or sign a .dmg')
    p_sign.add_argument('target', type=Path, help='an .app bundle or a .dmg')
    p_sign.add_argument('--identity', required=True,
                        help="signing identity, or '-' for an ad-hoc signature")
    p_sign.add_argument('--entitlements', type=Path, default=None,
                        help='entitlements plist, applied to the .app only')

    p_notarize = sub.add_parser('notarize', help='notarize and staple an .app or .dmg')
    p_notarize.add_argument('artifact', type=Path)
    p_notarize.add_argument('--keychain-profile', required=True,
                            help='profile name from `xcrun notarytool store-credentials`')
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == 'verify':
            ceiling = None
            if args.max_minimum_os:
                ceiling = _parse_version(args.max_minimum_os)
                if ceiling is None:
                    raise Failure(f"--max-minimum-os is not a version: {args.max_minimum_os}")
            return verify(args.bundle, ceiling)
        if args.command == 'prune':
            return prune(args.bundle)
        if args.command == 'sign':
            if args.target.is_dir():
                return sign(args.target, args.identity, args.entitlements)
            return sign_file(args.target, args.identity, args.entitlements)
        return notarize(args.artifact, args.keychain_profile)
    except Failure as error:
        print(f"macos-bundle: error: {error}", file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
