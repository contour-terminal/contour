# A2 — Extension Roots and Environment on Windows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A daemon extension's roots (`root <label> = config:<pointer>`) resolve on Windows — `C:\Users\me\repos`,
`C:/Users/me/repos`, `\\server\share\repos` — and `@repos`/`@worktrees` working directories, `path:<root>`
arguments and `file-read` are judged by the Windows path grammar with case-insensitive comparison, while POSIX
behaviour stays byte-identical. `childEnvironment()` folds variable names on Windows. The orchestrator guest
takes the host's Windows spelling of its roots and compares it with what git prints.

**Architecture:** One enum, `vtpty::PathStyle { Posix, Windows }`, names a path grammar. The directory opener —
the seam every root check already goes through — reports the grammar of the paths it takes
(`IDirectoryOpener::pathStyle()`: native openers answer `NativePathStyle`, test fakes answer what they were
built with). All string rules (absolute-and-normalised, containment, equality, join, split, volume root) live in
one pure, platform-independent unit, `vtextension/RootPathSyntax`, parameterised by `PathStyle`, so the Windows
rules are table-tested on Linux CI too and the POSIX rules on Windows. `RootResolution`, `FileReadService`,
`ArgumentKind` and `RootPointer` call that unit with the opener's style instead of hard-coding `'/'`.
`ChildEnvironment` gains an `EnvironmentNameCase` rule, native by default at the call sites. The guest gets
its own small `HostPaths` unit (it cannot link `vtpty`), whose style is detected from the spelling the host
handed it.

**Tech Stack:** C++23, `std::expected`, Catch2 (`vtextension_test`, `vtpty_test`, `orchestrator_test`,
`vthost_test`), `vtpty::isPlainWindowsName` / `dosPathOf` (`src/vtpty/WindowsPathRules.hpp`, phase A), MSVC STL
`std::filesystem::weakly_canonical` through `core::platform::FileSystem`, Git for Windows (measured fixtures).

**Spec:** `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md` (the gap recorded under
phase 2, "A — Windows directory handles", and in
`2026-11-23-orchestrator-windows-directory-handles.md`, "Out of scope"). Phase B's
`2026-11-23-orchestrator-windows-process-runner.md`, "Out of scope", raises the `childEnvironment()` half.

## Baseline (to be measured in Task 1)

- `vtextension_test` does not compile on Windows: `::popen` at `src/vtextension/DaemonGuest_test.cpp:708`, in
  `fileSystemTypeOf()`, which only feeds a benchmark's `std::println`. So no vtextension case has ever run on
  Windows; Task 1 records which fail before this phase touches anything.
- `vthost_test "DaemonConsentKey: the run that holds the socket clears what earlier runs there left"` fails on
  Windows at `DaemonConsentKey_test.cpp:138-140`. **Not this phase's cause** (see Decision 12): the key-file
  name is `fnv1a64(pathToUtf8(socket.lexically_normal()))`, which on Windows hashes `\other\socket` and
  `\run\user\1000\contour\default` instead of the slash spellings. FNV-1a of the POSIX spellings is
  `485cef47…` < `48e82114…`; of the Windows spellings `7f499109…` > `5d7d4477…`, so the in-memory store's
  `std::map` lists the two files in the opposite order and the test's ordered `std::vector` comparison fails.
  Production only prefix-matches, in any order. Task 12 fixes the test.

## Global Constraints

- Project rules in `AGENT.md`: C++23, `std::expected`, `enum class` over `bool` (new enums `: uint8_t`, zero is
  the POSIX/sensitive/default case), Doxygen on every new public declaration, `.hpp` with `#pragma once`, no
  C-style loops, no `NOLINT`, zero warnings under `-Werror`, data-driven tables over scattered `if`s.
- **POSIX behaviour is byte-identical.** Every POSIX expectation in the existing suites stays as it is. A
  `PathStyle::Posix` call returns exactly what the code returns today: same accepted set, same error
  enumerator, same spelling (no new normalisation on POSIX — `/srv/x/../y` is still left to `weaklyCanonical`).
  Linux cannot be built on this machine: Linux CI is the gate for the POSIX half.
- Never edit `vendor/core-cpp`.
- **Do not edit `docs/extensions.md`** (uncommitted user work there), nor `wit/` doc comments (they are copied
  into checked-in guest bindings). Collect doc text in "Docs for phase E" below.
- Do not edit the roadmap file (uncommitted user work there).
- Builds take minutes; use a 10-minute timeout. Commands:
  - `cmake --build --preset clangcl-release --target vtextension_test` → `out/build/clangcl-release/bin/vtextension_test.exe`
  - `cmake --build --preset clangcl-release --target vtpty_test` → `out/build/clangcl-release/bin/vtpty_test.exe`
  - `cmake --build --preset clangcl-release --target orchestrator_test` → `out/build/clangcl-release/bin/orchestrator_test.exe`
  - `cmake --build --preset clangcl-release --target vthost_test` → `out/build/clangcl-release/bin/vthost_test.exe`
- Pure units get tests with **both** styles on every platform; tests that touch the real filesystem through the
  native opener are guarded `#ifdef _WIN32` (Windows behaviour) or stay `#ifndef _WIN32` where they already are.
- After the last task: `/simplify`, then a `clang-format` pass over touched files.
- Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

## Decisions

1. **The grammar is a value, `vtpty::PathStyle`, reported by the directory opener — not `#ifdef _WIN32` in the
   rules.** The opener is already passed to every function that judges a working directory or a path argument,
   so `opener.pathStyle()` adds no parameter there; pure functions without an opener (`mostSpecificRoot`,
   `checkRootsDistinct`, `resolveRoots`, `argumentAccepts`) take the style explicitly. This keeps every existing
   test (built on a default, POSIX-styled `FakeDirectoryOpener`) running unchanged on Windows, and lets a
   Windows-styled fake run the Windows rules on Linux. `PathStyle::Windows` implies `PathCase::Insensitive`.
2. **Windows grammar of a configured root** (user-written, trusted, then canonicalised and shown in the
   consent dialog):
   - accepted: drive-absolute `X:\…` or `X:/…` (any ASCII letter), UNC `\\server\share\…` or `//server/share/…`,
     `~`, `~\…`, `~/…` (home from `core::platform::homeDirectory`);
   - normalised on input: every `/` becomes `\`, the drive letter is upper-cased, trailing separators are
     dropped except a drive root's own (`C:\`); the result is canonicalised (`weaklyCanonical`), stripped of
     `\\?\` by `dosPathOf()`, and **re-validated** — a canonical form outside the grammar (`\\?\Volume{…}\`)
     is refused `NotAbsolute`;
   - refused: drive-relative `C:x` and rooted-without-drive `\x` (`NotAbsolute`); `\\server` without a share
     (`NotAbsolute`); `\\?\`, `\\.\`, `\??\` in either separator (`DevicePath`, new); any component that
     `isPlainWindowsName()` refuses — `.`, `..`, empty (`C:\a\\b`), a stream `a:b`, a reserved device name, a
     trailing dot or space, `<>"|?*`, control characters (`NotPlainName`, new). Unlike POSIX, `..` is refused
     rather than collapsed: Win32 collapses it before reparse points, POSIX after symlinks, and one rule per
     platform for a user's own config is enough.
3. **Windows grammar of a guest-supplied path** (a `cwd`, a `path:<root>` argument): `X:\` or `\\server\share`
   then `\`-separated components each `isPlainWindowsName()`; no trailing separator except `X:\`; at most
   `MaxPathArgumentBytes` (4096). **Forward slashes are refused, not normalised**: the guest is handed the
   resolved backslash spelling and builds on it, so one spelling exists end to end, and a `/` here is a guest
   bug. (`isPlainWindowsName` already refuses `/` inside a component.)
4. **Comparison on Windows folds ASCII letters only**, as `vtpty::PathCase::Insensitive` does; a non-ASCII
   case difference does not match, so a path is refused rather than admitted by a folding NTFS may not share.
   Separators are not folded in comparisons (both sides are already `\`-normalised).
5. **Containment in `RootResolution` moves from `vtpty::isAtOrBeneath` (filesystem-path based) to the string
   rule `RootPathSyntax::isAtOrBeneath(path, directory, style)`.** The `std::filesystem` version cannot judge a
   POSIX-styled fake on Windows nor a Windows-styled one on Linux. `AgentSessionLog` keeps the `vtpty` rule; a
   native-style cross-check test (Task 3) pins the two to the same answers.
6. **One spelling from canonicalisation to the guest.** `ResolvedRoot::paths` holds the canonical DOS spelling
   (on-disk case, long names, junctions/symlinks and mapped/`subst` drives resolved — the same function,
   `GetFinalPathNameByHandleW`, behind both `std::filesystem::canonical` and phase A's `resolvedPath()`). The
   consent dialog shows it, the grant digest hashes it, the guest receives it, and handles are compared with it.
7. **The grant digest hashes the resolved spelling, not a case-folded key.** Re-spelling a config
   (`c:/users/me/repos` vs `C:\Users\me\repos`) does not change an existing directory's canonical spelling, so it
   does not re-ask; only the not-yet-existing tail of a `create` root keeps the user's case, and changing that
   re-asks. Re-asking is the safe failure; folding the digest would buy nothing.
8. **`checkRootsDistinct` compares Windows paths folded**, so `C:\WT` and `C:\wt` (two not-yet-created roots)
   are `EqualRoots`; and `resolveInRoot`'s "the root itself is not below it" check uses `samePath`, so a
   `path:<root>` argument spelling the root in another case is still the root (`OutsideRoots`), never "below".
9. **`file-read` paths stay `/`-separated on every platform** (it is the guest-facing contract); the host splits
   on `/`, and on Windows refuses any component `isPlainWindowsName()` refuses — in particular one holding `\`,
   which Win32 would read as a separator and so smuggle `..` past the split.
10. **Environment names fold on Windows, ASCII only, and a name with a non-ASCII byte is withheld when names
    fold.** Windows compares environment names with its Unicode upcase table (`gıt_dir` with a dotless ı
    upper-cases to `GIT_DIR`); folding only ASCII would let such a name past `GIT_`. Withholding every
    non-ASCII-named variable from a declared command on Windows is fail-closed and costs nothing real. A later
    layer replaces a name that differs only in case, **its own spelling winning**; inherited names that differ
    only in case collapse, last wins (phase B Decision 8's rule).
11. **The guest detects the style from the host's spelling** (`X:\…` or `\\…` is Windows, `/…` is POSIX): it
    is wasm32 everywhere, so there is no platform macro to read. Its own grammar accepts both styles' normalised
    absolute forms; the host remains the authority.
12. **`DaemonConsentKey_test.cpp:140` is fixed here only because it is one line** (Task 12, independent of the
    rest); it shares no cause with the root grammar.

## Review Focus

Security invariant: **a root grant never widens.** A directory, an argument or a file is admitted on Windows
only if it is admitted under the resolved spelling the user consented to.

- **Case-folding.** `C:\WT\x` against root `C:\wt` is inside (Tasks 3, 5); `C:\wt` and `c:\WT` are one root
  (`EqualRoots`, Task 5); a `path:<root>` value equal to the root in another case is `OutsideRoots`, not
  "strictly below" (Task 5, "naming the root itself in another case"); nested roots fence each other across
  case (Task 7). Non-ASCII case differences never match (Task 3 rows `C:\Ä` vs `C:\ä`).
- **8.3 short names (`C:\PROGRA~1`).** A configured root canonicalises to the long name (Task 6 native test);
  a guest path through a short name is opened and judged by the handle's long `resolvedPath()` (Task 11). A
  not-yet-existing `create` component that later turns out to be some directory's short name fails closed: the
  root's handle resolves elsewhere and `openInRoot` refuses it.
- **Trailing dots and spaces, streams, device names.** `C:\repos.`, `C:\repos `, `C:\repos:x`, `C:\con` are
  refused at the grammar in both configured roots and guest paths (Tasks 3, 6), never handed to Win32 to
  silently strip.
- **Mixed separators.** Configured: normalised to `\` before canonicalisation (Task 6). Guest-supplied: a `/`
  anywhere is refused (Tasks 3, 5). `file-read`: `\` inside a component is refused (Task 7).
- **Junction-resolved vs as-written.** Roots are compared only in their canonical form, handles only by
  `resolvedPath()`; both come from `GetFinalPathNameByHandleW`. Check that no comparison mixes an as-written
  spelling with a resolved one (`FileReadService::openDirectory`, `resolveInRoot`, `mostSpecificRoot`). A
  junction inside a root pointing outside it is `OutsideRoots` (Task 11).
- **UNC.** `\\srv\share` is a volume root; `\\srv\other` is not beneath `\\srv\share`; `\\SRV\Share\x` is
  beneath `\\srv\share`; `\\srv` alone is refused; `\\?\` and `\\.\` are `DevicePath` (Tasks 3, 6).
- **A configured root and a resolved handle path are compared by the same rule** (`samePath` /
  `isAtOrBeneath` of `RootPathSyntax`, the opener's style), everywhere a root is used.
- **Environment.** `Git_Dir`, `contour_pane`, `gh_repo`, `gıt_dir` are withheld on Windows; nothing changes on
  POSIX (Task 8).
- **POSIX parity.** Every pre-existing POSIX assertion is untouched; diff the test files to confirm that only
  additions and the mechanical `PathStyle::Posix` arguments appear.

---

### Task 1: `vtextension_test` builds on Windows; record the baseline

**Files:**
- Modify: `src/vtextension/DaemonGuest_test.cpp` (`fileSystemTypeOf`, around line 704)

- [ ] **Step 1:** Guard the body of `fileSystemTypeOf()` with `#ifdef __linux__`; the `#else` branch returns
  `"unknown"`. Comment at the guard: `findmnt` is util-linux, absent on macOS and Windows, and the value is only
  printed beside a benchmark figure. Keep the signature; the call site at line 967 is unchanged.
- [ ] **Step 2:** Build: `cmake --build --preset clangcl-release --target vtextension_test` → links.
- [ ] **Step 3:** Run `out/build/clangcl-release/bin/vtextension_test.exe --reporter compact 2>&1 | Out-File out/a2-vtextension-baseline.txt`
  and list the failing case names (`vtextension_test.exe --list-tests` for the totals). These are the
  pre-existing Windows failures; later tasks must not add to them, and should remove those under
  `[daemon][roots]`, `[daemon][processes]` and `[file-read]` that this phase is about. Also record
  `vthost_test.exe "DaemonConsentKey*"` (expect the one failure at line 138).
- [ ] **Step 4:** Commit:
  ```
  test(vtextension): build the suite on Windows

  findmnt is util-linux; its answer is only printed beside a benchmark figure.

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

### Task 2: `vtpty::PathStyle`, and the opener names its grammar

**Files:**
- Create: `src/vtpty/PathStyle.hpp`
- Modify: `src/vtpty/DirectoryHandle.hpp`, `src/vtpty/DirectoryHandle_unix.cpp`, `src/vtpty/DirectoryHandle_win32.cpp`,
  `src/vtpty/DirectoryHandle_test.cpp`, `src/vtextension/testing/FakeDirectoryOpener.hpp`,
  `src/vtextension/FileReadService_test.cpp` (`RecordingOpener`, line 313)

- [ ] **Step 1: test first** — in `DirectoryHandle_test.cpp`:
  `TEST_CASE("DirectoryHandle: the native opener speaks the platform's path grammar", "[vtpty][directory]")`
  → `CHECK(vtpty::nativeDirectoryOpener().pathStyle() == vtpty::NativePathStyle);` plus
  `STATIC_CHECK(vtpty::NativePathStyle == vtpty::PathStyle::Windows)` under `#ifdef _WIN32`, `…Posix` otherwise.
  Build fails (no `pathStyle`).
- [ ] **Step 2:** `PathStyle.hpp`:
  ```cpp
  /// The grammar a path is spelled in: its separator, what makes it absolute, and how components compare.
  enum class PathStyle : uint8_t
  {
      Posix = 0, ///< `/`-separated, absolute from `/`, compared byte for byte.
      Windows,   ///< `\`-separated, absolute from `X:\` or `\\server\share`, ASCII letters folded.
  };
  /// The platform's own grammar.
  inline constexpr auto NativePathStyle = /* Windows under _WIN32, Posix otherwise */;
  ```
- [ ] **Step 3:** `IDirectoryOpener`: `/// @return The grammar of the paths this opener takes and resolves.`
  `[[nodiscard]] virtual PathStyle pathStyle() const noexcept = 0;`. Both native openers return
  `NativePathStyle`. `FakeDirectoryOpener`: a default constructor (POSIX, today's behaviour) and
  `explicit FakeDirectoryOpener(vtpty::PathStyle style)`, stored in a private `_style`, returned by
  `pathStyle()` (Windows behaviour of the fake comes in Task 5). `RecordingOpener` returns `PathStyle::Posix`.
- [ ] **Step 4:** Build `vtpty_test` and `vtextension_test`; run `vtpty_test.exe "DirectoryHandle*"` → passes;
  `vtextension_test.exe` → same failures as the Task 1 baseline.
- [ ] **Step 5:** Commit `feat(vtpty): a directory opener names the path grammar it speaks` (+ trailer).

### Task 3: `RootPathSyntax` — the string rules of a root, for both grammars

**Files:**
- Create: `src/vtextension/RootPathSyntax.hpp`, `src/vtextension/RootPathSyntax.cpp`, `src/vtextension/RootPathSyntax_test.cpp`
- Modify: `src/vtextension/CMakeLists.txt` (library sources; `vtextension_test` sources)

Signatures (all pure, UTF-8 in and out, no filesystem):

```cpp
/// The longest path a Path argument, a working directory or a root may have, in bytes.
inline constexpr std::size_t MaxPathArgumentBytes = 4096;   // moved here from ArgumentKind.hpp

/// @return Whether @p path starts with the style's absolute prefix: `/` (Posix); `X:\` or `\\` (Windows).
[[nodiscard]] bool hasAbsoluteRoot(std::string_view path, vtpty::PathStyle style) noexcept;

/// @return Whether @p path is absolute and normalised by @p style (Decision 3 for Windows; for Posix exactly
///         today's detail::isNormalisedAbsolutePath()).
[[nodiscard]] bool isNormalisedAbsolutePath(std::string_view path, vtpty::PathStyle style) noexcept;

/// @return Whether @p component is one name of @p style: Posix non-empty, not `.`/`..`, no `/`, no NUL;
///         Windows vtpty::isPlainWindowsName().
[[nodiscard]] bool isPlainComponent(std::string_view component, vtpty::PathStyle style) noexcept;

/// @return Whether @p path is a volume root: `/`; `X:\`, `\\server\share`.
[[nodiscard]] bool isVolumeRoot(std::string_view path, vtpty::PathStyle style) noexcept;

/// @return @p path's parent and last component; std::nullopt for a volume root.
[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>> parentAndLeaf(
    std::string_view path, vtpty::PathStyle style) noexcept;

/// @return @p component appended to @p directory with the style's separator (none added after a volume root
///         that ends in one).
[[nodiscard]] std::string childPath(std::string_view directory, std::string_view component, vtpty::PathStyle style);

/// @return Whether @p a and @p b name the same path: byte-equal (Posix); ASCII-folded (Windows).
[[nodiscard]] bool samePath(std::string_view a, std::string_view b, vtpty::PathStyle style) noexcept;

/// @return Whether normalised @p path is @p directory or lies beneath it, by whole components, by samePath's
///         rule. (`/wt/ab` is not beneath `/wt/a`; `D:\w` not beneath `C:\w`; `\\s\b` not beneath `\\s\a`.)
[[nodiscard]] bool isAtOrBeneath(std::string_view path, std::string_view directory, vtpty::PathStyle style) noexcept;

/// @return The components of @p path strictly beneath @p directory, top-down; std::nullopt when it is not.
[[nodiscard]] std::optional<std::vector<std::string_view>> componentsBeneath(
    std::string_view directory, std::string_view path, vtpty::PathStyle style);

/// @return A key equal for exactly the paths samePath() calls equal (Posix: @p path; Windows: ASCII-lowered).
[[nodiscard]] std::string comparisonKeyOf(std::string_view path, vtpty::PathStyle style);
```

- [ ] **Step 1: tests first**, all platforms, `[vtextension][roots]`:
  - `"RootPathSyntax: normalised absolute paths, POSIX"` — the rows of today's
    `ArgumentKind_test` Path rows, plus `C:\x` → false.
  - `"RootPathSyntax: normalised absolute paths, Windows"` — accepted: `C:\`, `c:\x`, `C:\Users\me\wt\contour\0193`,
    `\\srv\share`, `\\srv\share\x`, `C:\PROGRA~1\x`; refused: `/x`, `C:`, `C:x`, `\x`, `C:/x`, `C:\x/y`, `C:\x\`,
    `C:\\x`, `C:\x\..\y`, `C:\x\.\y`, `C:\x:y`, `C:\con`, `C:\nul.txt`, `C:\x.`, `C:\x `, `\\srv`, `\\srv\`,
    `\\?\C:\x`, `\\.\pipe\x`, `\\srv\share\`, a 4097-byte path, an embedded NUL.
  - `"RootPathSyntax: containment and equality"` — rows `{path, directory, style, expected}`: POSIX `/w/a` in
    `/w` true, `/wx` in `/w` false, `/` in `/` true, `/W` in `/w` false; Windows `C:\W\a` in `C:\w` true,
    `C:\wx` in `C:\w` false, `D:\w` in `C:\w` false, `C:\x` in `C:\` true, `\\SRV\Share\x` in `\\srv\share` true,
    `\\srv\other` in `\\srv\share` false, `C:\Ä\x` in `C:\ä` false. `samePath("C:\\WT", "c:\\wt", Windows)` true,
    `samePath("/WT", "/wt", Posix)` false.
  - `"RootPathSyntax: join and split"` — `childPath("C:\\", "x", Windows) == "C:\\x"`,
    `childPath("C:\\a", "x", Windows) == "C:\\a\\x"`, `childPath("/", "x", Posix) == "/x"`,
    `parentAndLeaf("C:\\a", Windows) == pair{"C:\\", "a"}`, `parentAndLeaf("\\\\s\\sh\\a", Windows) == pair{"\\\\s\\sh", "a"}`,
    `parentAndLeaf("C:\\", Windows) == nullopt`, `parentAndLeaf("/a", Posix) == pair{"/", "a"}`;
    `componentsBeneath("C:\\wt", "c:\\WT\\r\\x", Windows) == {"r","x"}`, `componentsBeneath("C:\\wt", "C:\\wt", Windows)`
    `== nullopt`.
  - `#ifdef _WIN32` / `#else` `"RootPathSyntax: agrees with vtpty::isAtOrBeneath in the native grammar"` — for
    every native-style containment row above, `isAtOrBeneath(p, d, NativePathStyle) ==`
    `vtpty::isAtOrBeneath(pathFromUtf8(p), pathFromUtf8(d), vtpty::NativePathCase)`.
- [ ] **Step 2:** Implement; Windows components through `vtpty::isPlainWindowsName`; the UNC prefix's server and
  share are checked as components. Table-driven where it splits by style.
- [ ] **Step 3:** Build; `vtextension_test.exe "RootPathSyntax*"` → all pass.
- [ ] **Step 4:** Commit `feat(vtextension): the path grammar of a root, for POSIX and Windows` (+ trailer).

### Task 4: A Path argument is judged by the opener's grammar

**Files:**
- Modify: `src/vtextension/ArgumentKind.hpp`, `src/vtextension/ArgumentKind_test.cpp`,
  `src/vtextension/DeclaredArguments.cpp:45`, `src/vtextension/RootResolution.cpp` (`checkSyntax`, line 56)

- [ ] **Step 1: tests first** — `ArgumentKind_test.cpp`: every row and `static_assert` passes
  `vtpty::PathStyle::Posix` (mechanical); add `TEST_CASE("argumentAccepts: a Windows Path", "[vtextension][manifest]")`
  with `C:\wt\x` accepted and `/wt/x`, `C:/wt/x`, `C:\wt\..\x` refused under `PathStyle::Windows`, and
  `C:\wt\x` refused under `Posix`.
- [ ] **Step 2:** `[[nodiscard]] constexpr bool argumentAccepts(ArgumentKind kind, std::string_view value,
  vtpty::PathStyle style) noexcept;` — the Path case calls `isNormalisedAbsolutePath(value, style)`; the other
  kinds ignore @p style. It stays `constexpr` (C++23 permits the non-constexpr call on the Path branch; the
  `static_assert`s exercise the others). `detail::isNormalisedAbsolutePath` is removed (moved in Task 3);
  `MaxPathArgumentBytes` is reached through `RootPathSyntax.hpp`. Doxygen: `@param style The grammar a Path
  is judged by; the opener's.`
- [ ] **Step 3:** `DeclaredArguments.cpp:45` passes `directories.pathStyle()`. `checkSyntax(path, style)` in
  `RootResolution.cpp`: `!hasAbsoluteRoot(path, style)` → `NotAbsolute`; `!argumentAccepts(Path, path, style)` →
  `Unnormalized`; its callers pass `opener.pathStyle()`. Update `RootError::NotAbsolute`'s doc to "The path is
  not absolute in the opener's grammar."
- [ ] **Step 4:** Build; `vtextension_test.exe "argumentAccepts*"` and `"[processes]"` → pass (minus baseline).
- [ ] **Step 5:** Commit `feat(vtextension): judge a path argument by the opener's grammar` (+ trailer).

### Task 5: `RootResolution` in the opener's grammar; a Windows-styled fake opener

**Files:**
- Modify: `src/vtextension/RootResolution.hpp`, `src/vtextension/RootResolution.cpp`,
  `src/vtextension/testing/FakeDirectoryOpener.hpp`, `src/vtextension/RootResolution_test.cpp`,
  callers `src/vtextension/DeclaredPanes.cpp:153`, `src/vtextension/DeclaredProcesses.cpp:199`,
  `src/vtextension/FileReadService.cpp:131`, `src/vtextension/RootPointer.cpp:134`

Signature changes (the rest take the style from the opener):

```cpp
[[nodiscard]] std::optional<std::string_view> mostSpecificRoot(std::span<ResolvedRoot const> roots,
                                                               std::string_view resolvedPath,
                                                               vtpty::PathStyle style);
[[nodiscard]] std::expected<void, RootError> checkRootsDistinct(std::span<ResolvedRoot const> roots,
                                                                vtpty::PathStyle style);
```

- [ ] **Step 1: the fake.** `FakeDirectoryOpener` in `PathStyle::Windows`: lookups through `comparisonKeyOf`
  (so `c:\WT` finds `C:\wt` and answers the stored spelling, as `GetFinalPathNameByHandleW` answers the on-disk
  case); every `X:\` and `\\srv\share` volume root exists; `createBeneath` joins with `childPath`, refuses a name
  `isPlainComponent(…, Windows)` refuses with `AccessDenied` (phase A Decision 3). POSIX mode unchanged.
- [ ] **Step 2: tests first** in `RootResolution_test.cpp` (all platforms, `[daemon][processes]`; every
  existing case unchanged):
  - `"RootResolution: Windows most specific root, case-insensitively"` — roots
    `worktrees={C:\w}`, `repos={C:\r\contour, \\srv\share\r}`; rows `c:\W\a`→worktrees, `C:\wx`→none,
    `D:\w`→none, `\\SRV\Share\r\x`→repos, `\\srv\other\r`→none, `C:\`→none.
  - `"RootResolution: Windows roots equal but for case are refused"` —
    `checkRootsDistinct({{"a",{"C:\\WT"}},{"b",{"c:\\wt"}}}, Windows) == unexpected(EqualRoots)`; the same under
    `Posix` with `/WT`, `/wt` → ok.
  - `"RootResolution: a Windows path argument naming the root itself in another case is refused"` — root
    `worktrees={C:\wt\Work}`, `C:\wt` exists, `Work` missing: `resolvePathArgument(opener, roots, "worktrees",`
    `"C:\\wt\\work") == unexpected(OutsideRoots)`; with `C:\wt\Work` existing, value `C:\WT\WORK` → `OutsideRoots`.
  - `"RootResolution: a Windows create root is made beneath its drive"` — `ensureRoot` on
    `worktrees={C:\wt\a\b}` with only `C:\wt` existing → `opener.created == {"C:\\wt\\a", "C:\\wt\\a\\b"}`.
  - `"RootResolution: Windows path parents join with backslashes"` — `createPathParents(…, "worktrees",`
    `"C:\\wt\\contour\\run")` with `C:\wt` existing → `"C:\\wt\\contour\\run"`, `created == {"C:\\wt\\contour"}`.
  - `"RootResolution: Windows syntax refusals"` — `openInRoot` rows: `C:wt`, `\wt`, `C:/wt/x`, `/wt` →
    `NotAbsolute`; `C:\wt\..\x`, `C:\wt\x:y`, `C:\wt\con`, `C:\wt\x.`, `C:\wt\`, `\\?\C:\wt` → `Unnormalized`.
  - `"RootResolution: a Windows directory is judged by its resolved path"` — `C:\wt\link` resolves to `D:\x` →
    `OutsideRoots`; `c:\wt\A` (stored `C:\wt\a`) → ok, handle's `resolvedPath() == "C:\\wt\\a"`.
- [ ] **Step 3: implement.** Replace in `RootResolution.cpp`: `isInside` → `RootPathSyntax::isAtOrBeneath`;
  `existing == "/"` → `isVolumeRoot`; `rfind('/')`/`resize` → `parentAndLeaf`; the two `"/"`-joins
  (`ExistingAncestor::resolvedPath`, line 77; `createPathParents`, line 249) → `childPath`; `resolveInRoot`'s
  `std::ranges::contains(paths, resolved)` → `any_of(paths, samePath(…, resolved, style))` (Decision 8);
  `checkRootsDistinct` sorts `comparisonKeyOf` keys. `ExistingAncestor` carries the style it was opened with.
  Callers pass `directories.pathStyle()` / `_directories.pathStyle()`; `RootPointer.cpp:134` passes
  `vtpty::NativePathStyle` until Task 6 threads its parameter.
- [ ] **Step 4:** Build; `vtextension_test.exe "RootResolution*"`, `"[processes]"`, `"[file-read]"` → pass on
  Windows (POSIX-styled cases included — this is the first time they run there).
- [ ] **Step 5:** Commit `feat(vtextension): resolve and judge a root's directories in the opener's grammar` (+ trailer).

### Task 6: Configured roots on Windows

**Files:**
- Modify: `src/vtextension/RootPointer.hpp`, `src/vtextension/RootPointer.cpp`, `src/vtextension/RootPointer_test.cpp`,
  `src/vtextension/DaemonExtensionRuntime.cpp` (`RootPointerErrorTexts`, line 40; `resolveRoots` call, line 232)
- Possibly: `src/vtpty/DirectoryHandle_test.cpp` → `src/vtpty/testing/Junction.hpp` (see Step 4)

```cpp
enum class RootPointerError : uint8_t
{
    // … existing, unchanged order …
    DevicePath,  ///< A directory is a Win32 device or verbatim path: `\\?\`, `\\.\`, `\??\`.
    NotPlainName ///< A component of a directory is one Windows spells otherwise: `.`, `..`, empty, a stream,
                 ///< a device name, a trailing dot or space.
};

/// Spells a configured directory in @p style before it is resolved (Decision 2): `~` expanded; on Windows `/`
/// made `\`, the drive letter upper-cased, trailing separators dropped. Posix: exactly today's expansion.
/// @return The spelling; NoHome, NotAbsolute, DevicePath or NotPlainName.
[[nodiscard]] std::expected<std::string, RootPointerError> rootSpellingOf(
    std::string_view raw, std::string_view home, vtpty::PathStyle style);

[[nodiscard]] std::expected<std::string, RootPointerError> resolveRootPath(
    std::string_view raw, std::string_view home, core::platform::FileSystem const& fileSystem,
    vtpty::PathStyle style);

[[nodiscard]] std::expected<std::vector<ResolvedRoot>, RootPointerError> resolveRoots(
    Manifest const& manifest, std::string_view configJson, std::string_view home,
    core::platform::FileSystem const& fileSystem, vtpty::PathStyle style);
```

- [ ] **Step 1: tests first** (all platforms):
  `"RootPointer: a configured Windows directory is spelled once"` — `rootSpellingOf(raw, home, Windows)` rows
  (home `C:\Users\me`): `C:\Users\me\repos`→same; `c:/Users/me/repos`→`C:\Users\me\repos`; `C:\repos\`→`C:\repos`;
  `C:\`, `C:/`, `c:`+`\`→`C:\`; `\\srv\share\r`→same; `//srv/share/r`→`\\srv\share\r`; `\\srv\share`→same;
  `~`→`C:\Users\me`; `~\w`, `~/w`→`C:\Users\me\w`; `C:x`, `\x`, `x`, `\\srv`, `\\srv\`, `~bob\w`→`NotAbsolute`;
  `\\?\C:\x`, `//?/C:/x`, `\\.\pipe\x`, `\??\C:\x`→`DevicePath`; `C:\a\..\b`, `C:\a\.\b`, `C:\a\\b`, `C:\a:b`,
  `C:\repos.`, `C:\repos `, `C:\con`, `C:\a|b`→`NotPlainName`; `~\w` with home `""`→`NoHome`.
  `"RootPointer: a configured POSIX directory is spelled as before"` — `rootSpellingOf(…, Posix)`:
  `/srv/x/../y`→`/srv/x/../y`, `~/w`→`/home/u/w`, `C:\x`→`NotAbsolute`, `~\w`→`NotAbsolute`.
- [ ] **Step 2:** Existing `resolveRootPath`/`resolveRoots` cases pass `vtpty::PathStyle::Posix`. Run them on
  Windows; **only if** one fails because `InMemoryFileSystem` reads `/home/u` by Windows' `std::filesystem`
  grammar, guard that case `#ifndef _WIN32` with that reason in a comment (record which in the commit body).
- [ ] **Step 3: implement.** `resolveRootPath` = `rootSpellingOf` → `fileSystem.weaklyCanonical` → for Windows
  `dosPathOf(pathToUtf8(…))`, `rootSpellingOf(canonical, "", Windows)` again (any refusal → `NotAbsolute`);
  POSIX keeps today's trailing-`/` trim. `resolveRoots` threads @p style to `resolveRootPath` and
  `checkRootsDistinct`. `DaemonExtensionRuntime.cpp:232` passes `_services.directories.pathStyle()`. Two rows
  appended to `RootPointerErrorTexts`: `DevicePath` → "a root's directory is a device path (\\\\?\\ or \\\\.\\)",
  `NotPlainName` → "a root's directory has a component Windows spells otherwise (.., a stream, a device name,
  a trailing dot or space)"; `NotAbsolute`'s text becomes "a root's directory is not absolute (nor ~ or ~/...)"
  unchanged on POSIX — keep it, it reads for both.
- [ ] **Step 4: native Windows test**, `#ifdef _WIN32`:
  `"RootPointer: a Windows root resolves to the spelling its handle reads back"` — under a fresh
  `temp_directory_path()/contour-a2-<random>` with `NativeFileSystem` and `vtpty::nativeDirectoryOpener()`:
  create `Long Directory Name\inner`; for each spelling — upper-cased, `/`-separated, through a junction to it,
  through its 8.3 name from `GetShortPathNameW` (if that returns the long name, `WARN("8.3 names disabled")` and
  skip that row) — `resolveRootPath(spelling, "", fs, Windows) == opener.open(longPath)->resolvedPath()`.
  A not-yet-existing tail keeps its spelling: `resolveRootPath(dir + "\\New\\Sub")` ends with `\New\Sub`. Use
  phase A's junction helper; if it is file-local to `DirectoryHandle_test.cpp`, move it to
  `src/vtpty/testing/Junction.hpp` (one function, Doxygen'd) and include it from both.
- [ ] **Step 5:** Build; `vtextension_test.exe "RootPointer*"` and `"DaemonExtensionRuntime*"` → pass.
- [ ] **Step 6:** Commit `feat(vtextension): resolve a configured root on Windows` (+ trailer).

### Task 7: `file-read` in the opener's grammar

**Files:**
- Modify: `src/vtextension/FileReadService.cpp` (`componentsOf` 76, `pathBeneath` 92, `read` 131,
  `openDirectory` 146–175), `src/vtextension/testing/FakeBeneathReader.hpp` (joins at 55, 79),
  `src/vtextension/FileReadService_test.cpp`

- [ ] **Step 1: tests first** (all platforms, `[vtextension][file-read]`), with a Windows-styled
  `FakeDirectoryOpener` and `FakeBeneathReader`:
  - `"FileReadService: on Windows a component Windows spells otherwise is a bad argument"` — `read("worktrees",`
    `p)` for `p` in `a\..\..\x`, `STATUS.md:x`, `con`, `x.`, `x `, `a/b\c` → `BadArgument`; and under a POSIX
    opener `a\b` is still read (POSIX parity).
  - `"FileReadService: a Windows file beneath a readable root is read"` — root `C:\wt`, file
    `C:\wt\contour\r1\STATUS.md`, `read("worktrees", "contour/r1/STATUS.md")` → its bytes.
  - `"FileReadService: a Windows root read back in another case is the same directory"` — configured `C:\wt`,
    the fake resolves it to `C:\WT` → read succeeds (not `Escapes`); resolving to `D:\wt` → `Escapes`.
  - `"FileReadService: a Windows nested root in another case is still fenced"` — `worktrees={C:\wt}`,
    `repos={C:\WT\inner}` (readable `worktrees`) → `read("worktrees", "inner/x")` → `NotGranted`, and the walk's
    fence list holds `inner`.
- [ ] **Step 2: implement.** `componentsOf(path, style)`: split on `/` (the contract), each component
  `isPlainComponent(…, style)`. `pathBeneath` → `childPath` fold. `mostSpecificRoot(…, style)`.
  `openDirectory`: `samePath(handle->resolvedPath(), directory, style)`; nested roots via
  `componentsBeneath(directory, nested, style)`. Style is `_directories.pathStyle()`. `FakeBeneathReader`
  gets the same two constructors as the opener.
- [ ] **Step 3:** Build; `vtextension_test.exe "[file-read]"` → pass.
- [ ] **Step 4:** Commit `feat(vtextension): read a root's files in the opener's grammar` (+ trailer).

### Task 8: `childEnvironment()` folds names on Windows

**Files:**
- Modify: `src/vtextension/ChildEnvironment.hpp`, `src/vtextension/ChildEnvironment.cpp`,
  `src/vtextension/ChildEnvironment_test.cpp`, `src/vtextension/DeclaredProcesses.cpp:334`,
  `src/vtextension/DeclaredPanes.cpp:112`, `src/vtextension/DeclaredPanes_test.cpp`

```cpp
/// How environment variable names compare.
enum class EnvironmentNameCase : uint8_t
{
    Sensitive = 0, ///< Byte for byte, as on POSIX.
    Insensitive,   ///< ASCII letters fold, as Windows does; a name with a non-ASCII byte is withheld, since
                   ///< Windows folds it by a table this host does not share (Decision 10).
};
/// The platform's own rule: Insensitive on Windows, Sensitive elsewhere.
inline constexpr auto NativeEnvironmentNameCase = /* … */;

[[nodiscard]] Inheritance inheritanceOf(std::string_view name, EnvironmentNameCase rule) noexcept;
[[nodiscard]] std::vector<std::pair<std::string, std::string>> childEnvironment(
    std::span<std::string const> inherited, std::span<EnvironmentEntry const> commandEnvironment,
    std::span<EnvironmentEntry const> perCommand, EnvironmentNameCase rule);
```

- [ ] **Step 1: tests first** (all platforms, `[daemon][processes]`); existing cases pass `Sensitive`:
  - `"ChildEnvironment: names fold where the platform folds them"` — `Insensitive`: `Git_Dir`, `git_config_parameters`,
    `contour_pane`, `Gh_Repo`, `gitlab_host`, `Glab_Remote_Alias` withheld; `git_ssh_command`, `Git_Askpass`, `Path`,
    `gh_token` kept. `Sensitive`, same names: all kept (no rule matches) — today's behaviour.
  - `"ChildEnvironment: a non-ASCII name is withheld where names fold"` — `Insensitive`: `gıt_dir` (U+0131),
    `ÜBER` withheld; `Sensitive`: both kept.
  - `"ChildEnvironment: a later layer replaces a name that differs only in case"` — inherited `Path=C:\a`,
    command-env `PATH=C:\b` → `Insensitive`: exactly `{ {"PATH","C:\\b"} }`; `Sensitive`: both entries.
  - `"ChildEnvironment: inherited names that differ only in case collapse, last wins"` — `Insensitive`:
    `{"Tmp=a", "TMP=b"}` → `{ {"TMP","b"} }`.
  - `DeclaredPanes_test.cpp`, `#ifdef _WIN32`: `"paneWithheldNames folds names on Windows"` —
    `paneWithheldNames({"Git_Dir=x"}) == {"Git_Dir"}`.
- [ ] **Step 2: implement.** One `namesMatch(a, b, rule)` / `startsWithName(name, prefix, rule)` pair;
  `assign()` matches by `namesMatch` and replaces name and value. Call sites pass `NativeEnvironmentNameCase`.
  `SettableEnvironment`'s manifest-time checks stay exact (an allowlist: a re-cased name is not listed, so it is
  refused — fail-closed already; note it in the commit body).
- [ ] **Step 3:** Build; `vtextension_test.exe "ChildEnvironment*"`, `"GitLab's host selectors are withheld"`,
  `"[processes]"` → pass.
- [ ] **Step 4:** Commit `fix(vtextension): fold environment names on Windows` (+ trailer).

### Task 9: The orchestrator takes a Windows host's paths as the host spells them

**Files:**
- Create: `extensions/orchestrator/core/HostPaths.hpp`, `extensions/orchestrator/core/HostPaths.cpp`,
  `extensions/orchestrator/core/HostPaths_test.cpp`
- Modify: `extensions/orchestrator/CMakeLists.txt`, `core/ArgumentKinds.hpp` (`isNormalisedAbsolute`, 98),
  `core/ArgumentKinds_test.cpp`, `core/Config.hpp`/`Config.cpp` (`lastComponent` 554, `isInside` 343, the
  equality checks at 794, 1232), `core/Naming.hpp`/`Naming.cpp` (`worktreePath`, 105–113), `core/Engine.cpp`
  (316, 528), `core/EngineDispatch.cpp` (130, 172), `core/EnginePoll.cpp` (899, 965), `guest/Driver.cpp`
  (`relativeTo`, 102; 873), their tests

```cpp
namespace orchestrator
{
/// How the host spells its paths. The guest is wasm32 everywhere: it reads this from the spelling.
enum class PathStyle : uint8_t { Posix = 0, Windows };

/// @return Posix for `/…`, Windows for `X:\…` or `\\…`; std::nullopt for anything else.
[[nodiscard]] std::optional<PathStyle> pathStyleOf(std::string_view absolutePath) noexcept;
/// @return @p component appended to @p directory with the style's separator.
[[nodiscard]] std::string joinPath(std::string_view directory, std::string_view component, PathStyle style);
/// @return The last component of normalised absolute @p path.
[[nodiscard]] std::string_view lastComponent(std::string_view path, PathStyle style) noexcept;
/// @return Whether @p path lies strictly inside @p root, by whole components; ASCII-folded and with `/` and `\`
///         equal on Windows (git spells `C:/…`).
[[nodiscard]] bool isStrictlyInside(std::string_view path, std::string_view root, PathStyle style) noexcept;
/// @return Whether @p a and @p b name one path, by isStrictlyInside's rule.
[[nodiscard]] bool sameHostPath(std::string_view a, std::string_view b, PathStyle style) noexcept;
/// @return What follows @p prefix in @p text when @p text starts with it by sameHostPath's rule.
[[nodiscard]] std::optional<std::string_view> afterHostPath(std::string_view text, std::string_view prefix,
                                                           PathStyle style) noexcept;
/// @return @p path beneath @p root as a `/`-separated relative path, what `file-read` takes; std::nullopt
///         when @p path is not strictly inside @p root.
[[nodiscard]] std::optional<std::string> slashRelative(std::string_view root, std::string_view path, PathStyle style);
}
```

- [ ] **Step 1: tests first:**
  - `HostPaths_test.cpp`: `"HostPaths: style, join, last component"` (`C:\wt`→Windows, `\\s\sh`→Windows,
    `/wt`→Posix, `wt`→nullopt; `joinPath("C:\\wt","contour",Windows)=="C:\\wt\\contour"`;
    `lastComponent("C:\\src\\contour",Windows)=="contour"`); `"HostPaths: containment and equality"` (`C:/Users/U/wt/x`
    strictly inside `C:\Users\u\wt` under Windows; `C:\wtx` not inside `C:\wt`; `/WT/x` not inside `/wt` under
    Posix); `"HostPaths: a file-read path is always /-separated"` (`slashRelative("C:\\wt","C:\\wt\\contour\\r1\\STATUS.md",`
    `Windows)=="contour/r1/STATUS.md"`; outside → nullopt).
  - `ArgumentKinds_test.cpp`: Windows rows accepted (`C:\wt\contour\r1`, `\\s\sh\x`), refused (`C:\wt\`, `C:\a\..\b`,
    `C:/a`, `C:a`).
  - `Config_test.cpp`: `"Config: a Windows host's roots"` — worktree root `C:\Users\me\wt`, repo
    `C:\Users\me\src\contour` → repo named `contour`; repo `C:\USERS\me\wt\x` → `InsideWorktreeRoot`; repo equal
    to the worktree root in another case → `EqualRoots`; a POSIX repo beside a Windows worktree root →
    `NotAbsolutePath`.
  - `Naming_test.cpp`: `worktreePath("C:\\wt", "contour", id, PathStyle::Windows) == "C:\\wt\\contour\\" + id`.
  - `Engine_test.cpp`: with a Windows host info, the STATUS.md `ReadFile` path is `C:\wt\contour\<id>\STATUS.md`
    and `ReadGitFile`'s is `…\<id>\.git`.
  - `Driver_test.cpp`: with a Windows host info, the `file-read` the driver issues is
    `("worktrees", "contour/<id>/STATUS.md")`.
- [ ] **Step 2: implement.** `Config` derives `pathStyle()` from `worktree_root` once (`Config::pathStyle()`
  accessor, Doxygen'd) and refuses a repo of the other style. `worktreePath(root, repo, runId, PathStyle)` —
  callers pass `_config->pathStyle()`. `ArgumentKinds.hpp`'s `isNormalisedAbsolute` accepts either style's
  normalised absolute form, detected from the value (Decision 11). The Driver keeps the style beside
  `_worktreeRoot`.
- [ ] **Step 3:** Build `orchestrator_test` and `vtextension_test` (the latter re-embeds the guest);
  `orchestrator_test.exe` → all pass; `vtextension_test.exe "DaemonGuest*"` → no new failure.
- [ ] **Step 4:** Commit `feat(orchestrator): take a Windows host's paths as the host spells them` (+ trailer).

### Task 10: The orchestrator compares git's spelling of a Windows path

**Files:**
- Create: `extensions/orchestrator/core/testdata/git/worktree-list-windows.porcelain-z` (+ the `.git`-file and
  `rev-parse` outputs as test literals)
- Modify: `core/Run.hpp`/`Run.cpp` (`commonDirIsMain` 890, `gitFileIsIntact` 898, their callers 487, 600),
  `core/Reconcile.hpp`/`Reconcile.cpp` (207, `isUnder` 215–222, 339), `core/Run_test.cpp`, `core/Reconcile_test.cpp`

- [ ] **Step 1: measure** on this machine in a scratch repo under the scratchpad: `git worktree add <wt>`;
  `git -C <wt> rev-parse --path-format=absolute --git-common-dir`; the content of `<wt>\.git`;
  `git worktree list --porcelain -z`. Expected: forward slashes, `C:/…`. Scrub the user directory to
  `C:/Users/u` (`harness/scrub-fixture.py` if it applies) and save the listing as the fixture; record the
  `git --version` in the commit body.
- [ ] **Step 2: tests first:**
  - `Run_test.cpp`: `"gitFileIsIntact: a Windows worktree's .git names its main checkout with forward slashes"` —
    content `gitdir: C:/Users/u/src/contour/.git/worktrees/<id>\n`, main checkout `C:\Users\u\src\contour`,
    worktree `C:\Users\u\wt\contour\<id>` → true; main checkout `c:\users\u\src\contour` → true;
    `…/contour-evil/.git/…` → false; `D:/Users/…` → false. `"commonDirIsMain: git's Windows spelling"` —
    `C:/Users/u/src/contour/.git\n` against `C:\Users\u\src\contour` → true; `…/other/.git` → false. POSIX rows
    unchanged.
  - `Reconcile_test.cpp`: `"reconcile: a Windows listing matches its runs"` — the fixture's entries match runs
    whose `worktree` is the backslash spelling; an entry outside `C:\Users\u\wt` without an agent branch is left
    alone.
- [ ] **Step 3: implement** with `afterHostPath` / `sameHostPath` / `isStrictlyInside`; the functions take a
  `PathStyle` (callers pass `_config->pathStyle()`); the remainder after the main checkout accepts `/.git` and,
  on Windows, `\.git`.
- [ ] **Step 4:** Build; `orchestrator_test.exe` → all pass.
- [ ] **Step 5:** Commit `fix(orchestrator): compare git's spelling of a Windows path` (+ trailer).

### Task 11: A Windows root, end to end through the native opener

**Files:**
- Modify: `src/vtextension/RootResolution_test.cpp`, `src/vtextension/DaemonExtensionRuntime_test.cpp`

- [ ] **Step 1: tests** (`#ifdef _WIN32`; they should pass on first run — if one fails, the fault is in Tasks
  3–7, fix it there):
  - `"RootResolution: a Windows root, end to end through the native opener"` — temp tree `R\repos\contour`,
    `R\wt` (missing), junction `R\repos\contour\out` → `R\elsewhere`; roots from `resolveRoots` on
    `{"worktree_root":"<R with / and lower case>/wt","repos":[{"path":"<R>\\repos\\contour"}]}` with
    `NativeFileSystem`; then: `ensureRoot(…, "worktrees")` creates `R\wt`; `openInRoot(…, "repos", <upper-cased
    path of contour>)` → ok; `openInRoot(…, "repos", <contour>\out)` → `OutsideRoots`;
    `createPathParents(…, "worktrees", <wt>\contour\r1)` returns a backslash path whose parent exists;
    `resolvePathArgument(…, "worktrees", <wt with / >)` → `NotAbsolute`; the root itself in another case →
    `OutsideRoots`; through the 8.3 name of `repos` (when 8.3 names exist) → ok and judged by the long name.
  - `"DaemonExtensionRuntime: a Windows root is shown and granted by its resolved spelling"` — the consent
    question's directory line shows the canonical `C:\…` spelling; the grant digest is equal for configs
    spelling the same existing directory as `c:/…/repos` and `C:\…\Repos\` (Decision 7).
- [ ] **Step 2:** Build; `vtextension_test.exe "*Windows*"` → pass; full `vtextension_test.exe` → failures are
  a subset of the Task 1 baseline.
- [ ] **Step 3:** Commit `test(vtextension): a Windows root end to end` (+ trailer).

### Task 12 (independent): `DaemonConsentKey_test` does not depend on hash order

**Files:**
- Modify: `src/vthost/DaemonConsentKey_test.cpp:137-140`

- [ ] **Step 1:** Compare `std::set<std::string>(names.begin(), names.end())` with the set of the two expected
  names (comment: the in-memory store lists files by name, and a name's hash prefix depends on how the
  platform spells the socket path).
- [ ] **Step 2:** Build `vthost_test`; `vthost_test.exe "DaemonConsentKey*"` → all pass.
- [ ] **Step 3:** Commit `test(vthost): compare the consent key files as a set` (+ trailer).

## Out of scope — gaps to raise

- **`contour agent transcript/stats --cwd`** compares the worktree with the directories Claude Code's session
  log names (`src/contour/cli/AgentSessionLog.cpp:52`, `vtpty::isAtOrBeneath`, native). How Claude Code on
  Windows spells `cwd` in its logs (and how it encodes the project directory name) is unmeasured; phase E's
  end-to-end run should check it.
- **Phase B's runner receives DOS paths up to 4096 bytes** as the cwd and as `path:` arguments; `CreateProcessW`'s
  `lpCurrentDirectory` is limited to `MAX_PATH` unless long paths are enabled. Phase B should refuse (not
  truncate) a longer cwd.
- **`SettableEnvironment`'s manifest-time checks** compare names exactly on every platform (manifests parse
  alike everywhere); a manifest declaring `PATH` in `command-env` and `Path` in `command-env-for` is accepted,
  and on Windows the second replaces the first at run time (Task 8). Harmless, but worth a manifest-level
  "names differ only in case" refusal later.

## Docs for phase E

Text to add when `docs/extensions.md` and the WIT comments are next edited:

- `docs/extensions.md`, roots section: "On Windows a root's directory is drive-absolute (`C:\Users\me\repos` or
  `C:/Users/me/repos`), a UNC path (`\\server\share\repos`), `~`, or `~\…`/`~/…`. Forward slashes are read as
  backslashes. A drive-relative path (`C:repos`), a path rooted without a drive (`\repos`), a device or verbatim
  path (`\\?\…`, `\\.\…`), `.` or `..`, a stream (`a:b`), a reserved device name (`CON`, `NUL`, …) or a name
  ending in a dot or a space is refused. The directory is resolved once — junctions, symbolic links, mapped and
  `subst` drives and 8.3 short names followed — and that spelling is what the consent dialog shows, what the
  grant is recorded for, and what the extension is told. Paths compare ignoring the case of ASCII letters."
- `docs/extensions.md`, environment section: "On Windows variable names compare ignoring case, so `Git_Dir` is
  withheld as `GIT_DIR` is; a variable whose name holds a non-ASCII character is withheld from declared commands
  on Windows. Where two layers set names that differ only in case, the later layer's name and value win."
- `docs/extensions.md`, `path:<root>` and working directories: "On Windows such a path is spelled with
  backslashes only, as the host's roots are; a forward slash is refused."
- `wit/daemon/world.wit`, `record root.paths`: "absolute, normalised, symlinks resolved, in the host's native
  spelling (`/home/me/repos`; `C:\Users\me\repos`, `\\server\share\repos`)." On `proc-start`'s `cwd` and Path
  arguments: "in the spelling of the root it lies in." On `file-read`'s `path`: "relative, `/`-separated on every
  platform."
- `extensions/orchestrator/README.md`: "On Windows, `worktree_root` and `repos[].path` may be written
  `C:\…`, `C:/…` or `\\server\share\…`; the orchestrator builds worktree paths with the host's separator and
  reads git's forward-slash output against them."
