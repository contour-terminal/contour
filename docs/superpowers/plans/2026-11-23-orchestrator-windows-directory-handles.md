# Windows Directory Handles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `vtpty::nativeDirectoryOpener()` opens, resolves and creates directories on Windows, holding a
`HANDLE`, so that phases B (process runner), C (`file-read` beneath a root) and D (`pane-open`) have a held
directory to work from.

**Architecture:** `DirectoryHandle` keeps one shape and one API, but owns a platform type: `OwnedFd` on POSIX
(unchanged), `UniqueHandle` on Windows (moved out of `Win32Handle.hpp` into a `Windows.h`-free header so the
widely included `DirectoryHandle.hpp` does not drag `Windows.h` in). `open` is `CreateFileW` with
`FILE_FLAG_BACKUP_SEMANTICS`, its path read back with `GetFinalPathNameByHandleW`; `createBeneath` is
`NtCreateFile` relative to the parent's handle with `FILE_OPEN_REPARSE_POINT`, refusing name-surrogate reparse
points. The string rules (which component names are acceptable, the `\\?\` prefix) are a pure,
platform-independent unit, tested on Linux CI too.

**Tech Stack:** C++23, Win32 (`CreateFileW`, `GetFinalPathNameByHandleW`, `GetFileInformationByHandleEx`,
`GetFullPathNameW`), ntdll (`NtCreateFile`, `RtlNtStatusToDosError`) reached through the existing
`vtpty::ntdllFunction<>()` with `winternl.h` types, Catch2 (`vtpty_test`).

**Spec:** `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md` (phase 2, "A — Windows
directory handles").

## Baseline (measured 2026-11-23, this machine, `out/build/clangcl-release`, HEAD `12372fdb8`)

- `cmake --build --preset clangcl-release --target vtpty_test` → `ninja: no work to do.` (tree up to date).
- `out/build/clangcl-release/bin/vtpty_test.exe` → `All tests passed (5115 assertions in 207 test cases)`.
- `vtpty_test.exe "DirectoryHandle*"` → 1 case, 2 assertions ("opening is unsupported").
- `vthost_test.exe "pane-open is refused before any pane exists where no held directory can be entered"` →
  fails at `OwnedPanes_test.cpp:1171` `REQUIRE( opened.has_value() )` — the roadmap's pre-existing failure,
  caused by this phase's gap; Task 3 expects it to turn green.

## Global Constraints

- Project rules in `AGENT.md`: C++23, `std::expected`, `enum class` over `bool`, Doxygen on every new public
  declaration, `.hpp` headers with `#pragma once`, no C-style loops, no `NOLINT`, zero warnings under `-Werror`.
- Never edit `vendor/core-cpp`. Reuse `core::platform::NativeHandle` / `core::platform::InvalidHandle`
  (`vendor/core-cpp/src/core/platform/Types.hpp`) and `core::pathFromUtf8` / `core::pathToUtf8`
  (`core/Utf8Path.hpp`) for UTF-8 ⇄ UTF-16.
- POSIX behaviour is unchanged: `DirectoryHandle_unix.cpp` is not edited except where a type name moves; every
  POSIX test stays as it is and stays guarded `#ifndef _WIN32`. New Windows tests are guarded `#ifdef _WIN32`.
  Linux cannot be built on this machine: Linux CI is the gate for the POSIX half.
- `resolvedPath()` on Windows is UTF-8, DOS form, backslash-separated, without the `\\?\` prefix
  (`C:\Users\x\repo`, `\\server\share\x`): byte-equal to `core::pathToUtf8(std::filesystem::canonical(p))`.
- Every handle this opener makes is non-inheritable (no `SECURITY_ATTRIBUTES`, no `OBJ_INHERIT`) — the
  `O_CLOEXEC` analogue phase B relies on.
- Held directories are opened with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`: holding one must not
  stop it from being renamed or removed, as on POSIX.
- Build: `cmake --build --preset clangcl-release --target vtpty_test` (binary:
  `out/build/clangcl-release/bin/vtpty_test.exe`). Builds take minutes; use a 10-minute timeout.

## Decisions

1. **`NtCreateFile` relative to the parent handle, not `CreateFileW(canonical + "\" + name)` plus a post-open
   check.** The post-check can only close a handle; it cannot undo a `CreateDirectoryW` that a junction swapped
   into the parent path between the check and the create already sent elsewhere. A relative `NtCreateFile` with
   `FILE_OPEN_IF | FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT` creates-or-opens atomically in the held object,
   whatever its name is now — no TOCTOU window. It is reached as `ConPty.cpp` already reaches ntdll:
   `winternl.h` for `OBJECT_ATTRIBUTES`/`UNICODE_STRING`/`IO_STATUS_BLOCK` and the constants (all present in SDK
   10.0.26100), `vtpty::ntdllFunction<>()` for `NtCreateFile` and `RtlNtStatusToDosError` (no `ntdll.lib` link
   dependency, no DDK).
2. **Refuse name-surrogate reparse points (`IsReparseTagNameSurrogate`), not every reparse point.** Junctions,
   symlinks and volume mount points are surrogates and are refused (`NotADirectory`); cloud-file placeholders
   (OneDrive-redirected `Documents`, common on managed machines), dedup and WOF directories are real
   directories carrying a storage tag and are accepted. The roadmap and `BeneathRead_win32.cpp`'s comment say
   "any reparse point"; phase C should take the same rule from the helper Task 3 produces.
3. **Reserved names are refused in `createBeneath` beyond what POSIX refuses.** A relative NT open does no DOS
   name translation, so it would happily create `CON`, `trail.`, or `x::$INDEX_ALLOCATION`, names a later Win32
   consumer (phase B's `CreateProcessW` cwd) cannot address or addresses differently. All are `AccessDenied`, as
   POSIX's `""`, `.`, `..`, `/`, NUL are.
4. **`open()` and `canonical()` follow links**, like POSIX `open`/`realpath`; the resolved path read from the
   handle is the truth consumers judge.
5. **`DescriptorPaths` is ignored on Windows**: `GetFinalPathNameByHandleW` always answers, so there is no
   walking fallback to select.
6. **Accessors stay platform-specific**: `descriptor()` (POSIX only) and `nativeHandle()` (Windows only), so a
   Windows TU calling `descriptor()` is a compile error rather than a silent `-1`.

## Out of scope — a roadmap gap to raise

`vtextension`'s root grammar is POSIX-only: `RootPointer.cpp:98` and `RootResolution.cpp` (`checkSyntax`:
`path.starts_with('/')`, `rfind('/')`, `existing == "/"`) refuse `C:\...` as `NotAbsolute`. With this phase
`@repos`/`@worktrees` still do not resolve on Windows; the roadmap's claim that A unblocks them needs a further
phase (Windows path grammar in `vtextension`). Not done here.

## Review Focus

- **A junction (no privilege needed) or symlink in `createBeneath`'s name** — refused `NotADirectory`, and
  nothing is created in its target. Task 3, "a junction in the way".
- **The parent's name swapped for a junction after it was opened** — the child is created in the held
  directory, at its new name, never through the junction. Task 3, "follows the held parent, not its name".
- **Names Win32 spells differently from NT**: `x:stream`, `x::$INDEX_ALLOCATION`, `trail.`, `trail `, `CON`,
  `nul.txt`, `COM¹`, `a\b`, `<>"|?*`, control characters, non-UTF-8 bytes — `AccessDenied`, directory left
  empty. Task 1 (rule table) and Task 3 (end to end).
- **Case-insensitive names**: `createBeneath(parent, "X")` with `x` present opens `x` (one directory, resolved
  path spelled `x`); `open` with a differently cased spelling or forward slashes yields the true-case path.
  Tasks 2 and 3.
- **Paths longer than `MAX_PATH`** — opened (prefixed `\\?\` internally after `GetFullPathNameW`), resolved path
  without the prefix. Task 2, "a long path".
- **A held directory removed** (and re-made at its name) — `isRemoved()` true; `OwnedPanes_test` relies on it.
  Task 2 and Task 3's `vthost_test` check.
- **An unbacked parent** (`DirectoryHandle::unbacked`) given to `createBeneath` — `AccessDenied`, never an NT
  open relative to a null root (which would resolve the name against the object root). Task 3.

---

### Task 1: Windows path rules, pure and platform-independent

**Files:**
- Create: `src/vtpty/WindowsPathRules.hpp`, `src/vtpty/WindowsPathRules.cpp`
- Create: `src/vtpty/WindowsPathRules_test.cpp`
- Modify: `src/vtpty/CMakeLists.txt` (add `WindowsPathRules.cpp` to `vtpty_SOURCES` beside
  `WindowsCommandLine.cpp`, the header to `vtpty_HEADERS`, the test to `vtpty_test`'s unconditional list — it
  builds and runs on every platform, as `WindowsCommandLine` does)

**Interfaces:**
- Produces (namespace `vtpty`):
  ```cpp
  /// @param name One path component, UTF-8.
  /// @return Whether Win32 addresses @p name by exactly this spelling, as one component of an ordinary
  ///         directory: not empty, not `.` or `..`; none of `<>:"/\|?*` or U+0000..U+001F; not ending in `.`
  ///         or ` `; its stem (up to the first `.`, trailing spaces dropped) not a reserved device name,
  ///         compared ASCII-case-insensitively.
  [[nodiscard]] bool isPlainWindowsName(std::string_view name) noexcept;

  /// @param finalPath A path as GetFinalPathNameByHandleW(VOLUME_NAME_DOS) spells it, UTF-8.
  /// @return It without the verbatim prefix: `\\?\C:\a` → `C:\a`, `\\?\UNC\s\sh\a` → `\\s\sh\a`; anything
  ///         else unchanged.
  [[nodiscard]] std::string dosPathOf(std::string_view finalPath);

  /// @param fullPath An absolute path as GetFullPathNameW spells it, UTF-8.
  /// @return It with the verbatim prefix, so no MAX_PATH limit applies: `C:\a` → `\\?\C:\a`,
  ///         `\\s\sh\a` → `\\?\UNC\s\sh\a`; a path already starting `\\?\` or `\\.\` unchanged.
  [[nodiscard]] std::string verbatimPathOf(std::string_view fullPath);
  ```
- The reserved stems are one `constexpr std::array` in the `.cpp` (data-driven: a new device name is a row):
  `CON PRN AUX NUL CONIN$ CONOUT$ COM0..COM9 COM¹ COM² COM³ LPT0..LPT9 LPT¹ LPT² LPT³` (superscripts as UTF-8).

- [ ] **Step 1: Write the failing tests** in `WindowsPathRules_test.cpp`, tag `[windowspathrules]`:

```cpp
TEST_CASE("isPlainWindowsName accepts ordinary names", "[windowspathrules]")
{
    for (auto const name: { "x", "repo-1", "a.b", ".git", "CONX", "con-d", "COM10", "LPT", "\xc3\x9c" "ber",
                            " lead", "a b" })
        CHECK(vtpty::isPlainWindowsName(name));
}

TEST_CASE("isPlainWindowsName refuses names Win32 spells differently", "[windowspathrules]")
{
    using namespace std::string_view_literals;
    for (auto const name: { ""sv, "."sv, ".."sv, "a/b"sv, "a\\b"sv, "x:stream"sv, "x::$INDEX_ALLOCATION"sv,
                            "C:"sv, "trail."sv, "trail "sv, "a<b"sv, "a>b"sv, "a\"b"sv, "a|b"sv, "a?b"sv,
                            "a*b"sv, "a\0b"sv, "\x01"sv, "a\x1f"sv, "CON"sv, "con"sv, "Nul.txt"sv,
                            "nul .txt"sv, "AUX"sv, "PRN.x.y"sv, "COM1"sv, "com9"sv, "LPT0"sv,
                            "COM\xc2\xb9"sv, "CONIN$"sv, "conout$"sv })
    {
        CAPTURE(name);
        CHECK_FALSE(vtpty::isPlainWindowsName(name));
    }
}

TEST_CASE("dosPathOf drops the verbatim prefix", "[windowspathrules]")
{
    CHECK(vtpty::dosPathOf(R"(\\?\C:\a\b)") == R"(C:\a\b)");
    CHECK(vtpty::dosPathOf(R"(\\?\UNC\srv\share\a)") == R"(\\srv\share\a)");
    CHECK(vtpty::dosPathOf(R"(C:\a)") == R"(C:\a)");
    CHECK(vtpty::dosPathOf(R"(\\srv\share)") == R"(\\srv\share)");
}

TEST_CASE("verbatimPathOf adds the verbatim prefix", "[windowspathrules]")
{
    CHECK(vtpty::verbatimPathOf(R"(C:\a)") == R"(\\?\C:\a)");
    CHECK(vtpty::verbatimPathOf(R"(\\srv\share\a)") == R"(\\?\UNC\srv\share\a)");
    CHECK(vtpty::verbatimPathOf(R"(\\?\C:\a)") == R"(\\?\C:\a)");
    CHECK(vtpty::verbatimPathOf(R"(\\.\pipe\x)") == R"(\\.\pipe\x)");
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test`
Expected: compile error, `'vtpty/WindowsPathRules.hpp' file not found`.

- [ ] **Step 3: Implement the three functions** in `WindowsPathRules.cpp`. Device-name match: take the stem
  up to the first `.`, drop trailing spaces, compare to each table row with an ASCII case fold
  (`std::ranges::equal` with a folding projection); the superscript rows compare byte-exact after folding the
  ASCII prefix.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[windowspathrules]"`
Expected: `All tests passed (… assertions in 4 test cases)`.

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/WindowsPathRules.hpp src/vtpty/WindowsPathRules.cpp src/vtpty/WindowsPathRules_test.cpp src/vtpty/CMakeLists.txt
git commit -m "feat(vtpty): the Windows path rules a directory handle obeys

Which component names Win32 addresses by their own spelling, and the
verbatim prefix, as a pure unit tested on every platform.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `DirectoryHandle` holds a `HANDLE`; `open`, `canonical`, `isRemoved` on Windows

**Files:**
- Create: `src/vtpty/UniqueHandle.hpp` (no `Windows.h`), `src/vtpty/UniqueHandle.cpp` (Windows only)
- Modify: `src/vtpty/Win32Handle.hpp` (delete its `UniqueHandle` class; `#include <vtpty/UniqueHandle.hpp>`)
- Modify: `src/vtpty/DirectoryHandle.hpp`, `src/vtpty/DirectoryHandle_win32.cpp`
- Modify: `src/vtpty/CMakeLists.txt` (`UniqueHandle.hpp` in `vtpty_HEADERS`; `UniqueHandle.cpp` appended in the
  existing `elseif(WIN32)` block that appends `ConPty.cpp`)
- Test: `src/vtpty/DirectoryHandle_test.cpp`

**Interfaces:**
- Consumes: Task 1's `dosPathOf`, `verbatimPathOf`.
- Produces:
  ```cpp
  // UniqueHandle.hpp — same class, same name, same members as today's Win32Handle.hpp version, over
  // core::platform::NativeHandle; valid() compares with nullptr and core::platform::InvalidHandle; reset()
  // is defined in UniqueHandle.cpp (CloseHandle). The whole header is #ifdef _WIN32.
  class UniqueHandle { /* get(), valid(), reset(), move-only */ };

  // DirectoryHandle.hpp
  #ifdef _WIN32
  using OwnedDirectory = UniqueHandle; ///< What a DirectoryHandle owns here: a directory HANDLE.
  #else
  using OwnedDirectory = OwnedFd;      ///< What a DirectoryHandle owns here: a directory descriptor.
  #endif
  DirectoryHandle(OwnedDirectory directory, std::string resolvedPath) noexcept;
  #ifndef _WIN32
  [[nodiscard]] int descriptor() const noexcept;                               // unchanged
  #else
  /// @return The open directory's HANDLE, still owned; null when unbacked. Opened with
  ///         HeldDirectoryAccess, shared read/write/delete, not inheritable.
  [[nodiscard]] core::platform::NativeHandle nativeHandle() const noexcept;
  #endif
  ```
  `unbacked()` builds from a default `OwnedDirectory`. Doc comments of `isRemoved`, `makeDirectoryOpener` and
  `nativeDirectoryOpener` are rewritten for Windows (no "every call answers `Unsupported`"; `isRemoved` is told
  on Windows; `DescriptorPaths` ignored there). `DirectoryError::Unsupported` stays: fakes and
  `FileReadService`'s table still use it.
- `DirectoryHandle_win32.cpp` internals (anonymous namespace), shared with Task 3:
  - `constexpr DWORD HeldDirectoryAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;`
  - `DirectoryError directoryError(DWORD error)` — one `constexpr std::array` of `{DWORD, DirectoryError}` rows:
    `ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_INVALID_NAME, ERROR_BAD_NETPATH, ERROR_BAD_NET_NAME,
    ERROR_FILENAME_EXCED_RANGE, ERROR_CANT_RESOLVE_FILENAME, ERROR_NOT_READY` → `Missing`; `ERROR_DIRECTORY` →
    `NotADirectory`; anything else `AccessDenied` (mirrors the POSIX `ENOENT/ELOOP/ENAMETOOLONG`,`ENOTDIR` split).
  - `std::expected<UniqueHandle, DirectoryError> openFollowing(std::string_view path, DWORD access)` —
    `core::pathFromUtf8` (failure → `AccessDenied`), `GetFullPathNameW`, `verbatimPathOf`, then `CreateFileW(…,
    access, share all, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)`.
  - `std::expected<std::string, DirectoryError> finalPathOf(HANDLE)` — `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED
    | VOLUME_NAME_DOS)` sized by a first call, `core::pathToUtf8`, `dosPathOf`; failure → `AccessDenied`.
  - `std::optional<FILE_STANDARD_INFO> standardInfo(HANDLE)` (`GetFileInformationByHandleEx(FileStandardInfo)`).
  - `bool isRemovedHandle(HANDLE)` — `DeletePending || NumberOfLinks == 0`.
- Behaviour: `open(path)` = `openFollowing(path, HeldDirectoryAccess)`; not `Directory` → `NotADirectory`;
  removed → `Missing`; then `finalPathOf`. `canonical(path)` = `openFollowing(path, FILE_READ_ATTRIBUTES)` (a file
  is fine) then `finalPathOf`. `createBeneath` still answers `Unsupported` until Task 3.
  `DirectoryHandle::isRemoved()` = backed and `isRemovedHandle(nativeHandle())`.

- [ ] **Step 1: Write the failing tests.** In `DirectoryHandle_test.cpp`: move "a missing directory" and "a
  file" out of the `#ifndef _WIN32` block (they are platform-neutral, unchanged). Replace the `#else` case
  "opening is unsupported" with the cases below, inside `#ifdef _WIN32`, tag `[processrunner]` like their
  neighbours. Add a test-local helper
  `[[nodiscard]] bool createJunction(std::filesystem::path const& link, std::filesystem::path const& target)` —
  `CreateDirectoryW(link)`, open it `GENERIC_WRITE` with `FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT`,
  `DeviceIoControl(FSCTL_SET_REPARSE_POINT)` with a mount-point buffer declared locally (the layout of
  `REPARSE_DATA_BUFFER::MountPointReparseBuffer`: tag `IO_REPARSE_TAG_MOUNT_POINT`, substitute name
  `\??\<target>`, print name `<target>`, both NUL-terminated) — junctions need no privilege, so this attack is
  tested on every Windows machine. Symlink cases use `std::filesystem::create_directory_symlink(…, error)` and
  `SKIP("no symlink privilege")` when it fails. A second helper
  `[[nodiscard]] std::string canonicalOf(std::filesystem::path const& p)` returns
  `core::pathToUtf8(std::filesystem::canonical(p))`; `pathToUtf8` below is `core::pathToUtf8`.

```cpp
TEST_CASE("DirectoryHandle: open reads the canonical path from the handle", "[processrunner]")
{   // dir/real; open real spelled with forward slashes and upper-cased; then a junction dir/j -> real
    REQUIRE(opened.has_value());
    CHECK(opened->nativeHandle() != nullptr);
    CHECK(opened->resolvedPath() == canonicalOf(real));      // true case, backslashes
    CHECK_FALSE(opened->resolvedPath().starts_with(R"(\\?\)"));
    CHECK(viaJunction->resolvedPath() == canonicalOf(real));
    CHECK(opener.canonical(pathToUtf8(junction)) == canonicalOf(real));
    CHECK(vtpty::makeDirectoryOpener(vtpty::DescriptorPaths::Unavailable)->open(pathToUtf8(real))
              ->resolvedPath() == canonicalOf(real));
}

TEST_CASE("DirectoryHandle: open follows a directory symlink", "[processrunner]")
{   // SKIP without privilege
    CHECK(opened->resolvedPath() == canonicalOf(real));
}

TEST_CASE("DirectoryHandle: canonical resolves a file, and a missing path is Missing", "[processrunner]")
{
    CHECK(opener.canonical(pathToUtf8(file)) == canonicalOf(file));
    CHECK(opener.canonical(pathToUtf8(dir / "missing")).error() == vtpty::DirectoryError::Missing);
}

TEST_CASE("DirectoryHandle: a held directory may be renamed and removed, and tells it was removed",
          "[processrunner]")
{
    // open dir/a; rename a -> a.old succeeds while held; isRemoved() false;
    // std::filesystem::remove(a.old) succeeds while held; isRemoved() true
    CHECK_NOTHROW(std::filesystem::rename(a, aOld));
    CHECK_FALSE(held->isRemoved());
    CHECK(std::filesystem::remove(aOld));
    CHECK(held->isRemoved());
}

TEST_CASE("DirectoryHandle: a path longer than MAX_PATH", "[processrunner]")
{
    // create 6 nested 60-character components through a "\\?\"-prefixed path; open the plain spelling
    REQUIRE(longPath.native().size() > MAX_PATH);
    REQUIRE(opened.has_value());
    CHECK(opened->resolvedPath() == pathToUtf8(longPath));
}

TEST_CASE("DirectoryHandle: an unbacked handle holds no HANDLE", "[processrunner]")
{
    auto const handle = vtpty::DirectoryHandle::unbacked(R"(C:\somewhere)");
    CHECK(handle.nativeHandle() == nullptr);
    CHECK_FALSE(handle.isRemoved());
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test`
Expected: compile error, `no member named 'nativeHandle' in 'vtpty::DirectoryHandle'`.

- [ ] **Step 3: Move `UniqueHandle`** to `UniqueHandle.hpp`/`.cpp` and include it from `Win32Handle.hpp`; no
  consumer edits (same name, same namespace).

- [ ] **Step 4: Change `DirectoryHandle.hpp`** to `OwnedDirectory`, the platform accessors and the rewritten
  doc comments; implement `open`, `canonical` and `isRemoved` in `DirectoryHandle_win32.cpp` as specified
  above (`createBeneath` keeps answering `Unsupported`; rename the opener class `NativeDirectoryOpener`).

- [ ] **Step 5: Run to verify they pass, and nothing else regressed**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "DirectoryHandle*" && out/build/clangcl-release/bin/vtpty_test.exe`
Expected: the `DirectoryHandle*` cases all pass (one may report `skipped` without symlink privilege); the full
run `All tests passed` with 207 − 1 + 2 + 6 + 4 = 218 test cases (the 4 from Task 1). Then
`cmake --build --preset clangcl-release` (whole tree: `vthost`, `vtextension`, `contour` include the header)
builds without a warning, apart from `vtextension_test`'s pre-existing `popen` error.

- [ ] **Step 6: Commit**

```bash
git add src/vtpty/UniqueHandle.hpp src/vtpty/UniqueHandle.cpp src/vtpty/Win32Handle.hpp src/vtpty/DirectoryHandle.hpp src/vtpty/DirectoryHandle_win32.cpp src/vtpty/DirectoryHandle_test.cpp src/vtpty/CMakeLists.txt
git commit -m "feat(vtpty): open and resolve directories by handle on Windows

DirectoryHandle owns a HANDLE on Windows, opened with backup semantics and
shared for rename and delete; its path is read back from the handle, and
isRemoved() tells a held directory that was removed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `createBeneath` on Windows, relative to the held parent

**Files:**
- Create: `src/vtpty/Win32OpenBeneath.hpp`, `src/vtpty/Win32OpenBeneath.cpp` (Windows only; appended in the same
  `elseif(WIN32)` block of `src/vtpty/CMakeLists.txt`, header listed in `vtpty_HEADERS`)
- Modify: `src/vtpty/DirectoryHandle_win32.cpp`
- Modify: `src/vtpty/BeneathRead_win32.cpp` (the design comment only: point at `openBeneath()` and Decision 2's
  name-surrogate rule)
- Modify: `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md` (Step 6)
- Test: `src/vtpty/DirectoryHandle_test.cpp`

**Interfaces:**
- Consumes: Task 1's `isPlainWindowsName`; Task 2's `UniqueHandle`, `DirectoryHandle::nativeHandle()`,
  `HeldDirectoryAccess` (move it into `Win32OpenBeneath.hpp` as `inline constexpr DWORD`), `directoryError`,
  `finalPathOf`.
- Produces (phase C reuses it; namespace `vtpty`, `#ifdef _WIN32`, includes `Win32Handle.hpp`):
  ```cpp
  /// What openBeneath() opens. Phase C adds the entries a read needs.
  enum class BeneathEntry : uint8_t
  {
      DirectoryCreatedIfMissing = 0, ///< A directory, created when nothing has the name.
  };

  /// Opens one entry beneath an open directory, relative to its handle (NtCreateFile with a RootDirectory),
  /// following no reparse point: a link in the name's place is opened itself, never its target.
  /// @param parent An open directory; never null.
  /// @param name One component, UTF-16, already checked by isPlainWindowsName().
  /// @param entry What is opened.
  /// @param access The access the handle is opened with.
  /// @return The handle, not inheritable; or the Win32 error the NTSTATUS maps to (RtlNtStatusToDosError).
  [[nodiscard]] std::expected<UniqueHandle, DWORD> openBeneath(HANDLE parent, std::wstring_view name,
                                                               BeneathEntry entry, DWORD access);

  /// @return Whether @p handle is a name-surrogate reparse point (junction, symlink, mount point):
  ///         GetFileInformationByHandleEx(FileAttributeTagInfo); an unanswerable query counts as one.
  [[nodiscard]] bool isNameSurrogate(HANDLE handle) noexcept;
  ```
  `openBeneath` call: `OBJ_CASE_INSENSITIVE`, `RootDirectory = parent`, `FILE_SHARE_READ | FILE_SHARE_WRITE |
  FILE_SHARE_DELETE`, `FILE_ATTRIBUTE_NORMAL`, disposition `FILE_OPEN_IF`, options `FILE_DIRECTORY_FILE |
  FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT`, security descriptor null (inherits the parent's ACL,
  the analogue of `mkdir 0777` under the umask). The two ntdll functions are looked up once
  (`static auto* const`), a missing one answering `ERROR_CALL_NOT_IMPLEMENTED`.
- `createBeneath(parent, name)` order: unbacked parent → `AccessDenied`; `!isPlainWindowsName(name)` →
  `AccessDenied`; `core::pathFromUtf8(name)` fails → `AccessDenied`; `openBeneath` error → `directoryError(…)`
  (`ERROR_DIRECTORY` from `STATUS_NOT_A_DIRECTORY` is the file-in-the-way case → `NotADirectory`);
  `isNameSurrogate` → `NotADirectory`; then `finalPathOf(child)`.

- [ ] **Step 1: Write the failing tests** (in the `#ifdef _WIN32` block, using Task 2's helpers):

```cpp
TEST_CASE("DirectoryHandle: createBeneath creates one directory, and opens one already there",
          "[processrunner]")
{
    CHECK(std::filesystem::is_directory(dir / "x"));
    CHECK(created->resolvedPath() == canonicalOf(dir) + "\\x");
    CHECK(opener.createBeneath(*parent, "x").has_value());
    // case-insensitive: "X" opens "x"
    auto const upper = opener.createBeneath(*parent, "X");
    REQUIRE(upper.has_value());
    CHECK(upper->resolvedPath() == canonicalOf(dir) + "\\x");
    CHECK(std::ranges::distance(std::filesystem::directory_iterator { dir }) == 1);
}

TEST_CASE("DirectoryHandle: createBeneath refuses what stands in the way", "[processrunner]")
{
    // a file "file"; a junction "junction" -> dir/elsewhere (empty)
    CHECK(opener.createBeneath(*parent, "file").error() == vtpty::DirectoryError::NotADirectory);
    CHECK(opener.createBeneath(*parent, "junction").error() == vtpty::DirectoryError::NotADirectory);
    CHECK(std::filesystem::is_empty(dir / "elsewhere"));
}

TEST_CASE("DirectoryHandle: createBeneath refuses a directory symlink", "[processrunner]")
{   // SKIP without privilege
    CHECK(opener.createBeneath(*parent, "link").error() == vtpty::DirectoryError::NotADirectory);
}

TEST_CASE("DirectoryHandle: createBeneath refuses names Win32 spells differently", "[processrunner]")
{
    using namespace std::string_view_literals;
    for (auto const name: { ""sv, "."sv, ".."sv, "a/b"sv, "a\\b"sv, "x:stream"sv, "x::$INDEX_ALLOCATION"sv,
                            "trail."sv, "trail "sv, "CON"sv, "nul.txt"sv, "COM1"sv, "a?b"sv, "a\0b"sv,
                            "\xff"sv })
    {
        CAPTURE(name);
        CHECK(opener.createBeneath(*parent, name).error() == vtpty::DirectoryError::AccessDenied);
    }
    CHECK(std::filesystem::is_empty(dir));
    CHECK(opener.createBeneath(vtpty::DirectoryHandle::unbacked(canonicalOf(dir)), "x").error()
          == vtpty::DirectoryError::AccessDenied);
    CHECK(std::filesystem::is_empty(dir));
}

TEST_CASE("DirectoryHandle: createBeneath follows the held parent, not its name", "[processrunner]")
{
    // open dir/a; rename a -> a.old; createJunction(dir/a, dir/elsewhere); createBeneath(parent, "child")
    REQUIRE(created.has_value());
    CHECK(std::filesystem::is_directory(dir / "a.old" / "child"));
    CHECK(std::filesystem::is_empty(dir / "elsewhere"));
    CHECK(created->resolvedPath() == canonicalOf(dir / "a.old") + "\\child");
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "DirectoryHandle: createBeneath*"`
Expected: FAIL — `createBeneath` answers `Unsupported` (`with expansion: 3 == 1` style mismatches).

- [ ] **Step 3: Implement `openBeneath`, `isNameSurrogate`** in `Win32OpenBeneath.cpp` and `createBeneath` in
  `DirectoryHandle_win32.cpp` as specified. If the create test fails with `AccessDenied` from
  `ERROR_ACCESS_DENIED` (a relative create checked against the parent handle's granted access), add
  `FILE_ADD_SUBDIRECTORY` to `HeldDirectoryAccess` and record why at the constant; do not widen it otherwise.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe`
Expected: `All tests passed` with 223 test cases (Task 2's 218 + 5).

- [ ] **Step 5: Check the downstream suite the roadmap lists as failing for this gap**

Run: `cmake --build --preset clangcl-release --target vthost_test && out/build/clangcl-release/bin/vthost_test.exe "pane-open is refused before any pane exists where no held directory can be entered"`
Expected: `All tests passed` (both sections: refused `NotSupported` through `HeldDirectorySpawn::Unsupported`, and
refused because `isRemoved()`). Then run `out/build/clangcl-release/bin/vthost_test.exe "[panes]"` and confirm
no case fails that did not fail at baseline (`DaemonConsentKey_test.cpp:140` stays a pre-existing failure).

- [ ] **Step 6: Update the roadmap's "Pre-existing Windows test failures"**: drop
  `OwnedPanes_test.cpp:1171`, and add under phase 2 one line naming the vtextension root-grammar gap (see "Out
  of scope" above) so it gets its own phase before B–D are relied on for the orchestrator.

- [ ] **Step 7: Commit**

```bash
git add src/vtpty/Win32OpenBeneath.hpp src/vtpty/Win32OpenBeneath.cpp src/vtpty/DirectoryHandle_win32.cpp src/vtpty/BeneathRead_win32.cpp src/vtpty/DirectoryHandle_test.cpp src/vtpty/CMakeLists.txt docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md
git commit -m "feat(vtpty): create a directory beneath a held one on Windows

NtCreateFile relative to the parent's handle creates or opens the child in
the held directory whatever its name is now; a junction, symlink or mount
point in the child's place is refused, as are names Win32 would spell
differently (streams, trailing dots, device names).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## After the last task

- Run `/simplify` over the branch diff (AGENT.md workflow) and rebuild `vtpty_test`.
- Report in the change summary: no performance impact (opener calls are per command, not per frame); risk —
  Windows-only code paths plus one header-level type alias on POSIX; coverage — the new Windows branches are
  exercised by the `DirectoryHandle*` cases, `WindowsPathRules` on every platform.
