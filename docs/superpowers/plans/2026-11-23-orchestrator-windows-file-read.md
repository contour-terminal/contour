# C — Windows `file-read` Beneath a Root Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `vtpty::nativeBeneathReader()` reads a regular file beneath a held directory on Windows with the POSIX
reader's contract — one component at a time relative to the handle above it, following no link, confined to the
root's volume, refusing second names and fenced directories, bounded by `maxBytes` — so that an extension's
`file-read` (the orchestrator's Board tab, `STATUS.md`) works on Windows.

**Architecture:** A Windows seam, `IWin32BeneathSystem` (Windows-only header, the analogue of the POSIX-shaped
`IBeneathSystem`): open one entry beneath a `HANDLE` through phase A's `openBeneath()` (two new `BeneathEntry`
rows), re-open an already-open entry for reading (`ReOpenFile`: the same object, no second name lookup), describe
a handle as a `FileStatus`, read from it. A Windows `IBeneathReader` walks the components over that seam with the
same steps and error vocabulary as `BeneathRead_unix.cpp`. The classification of what an entry *is* (regular,
directory, link, other) from its attributes, reparse tag and handle type, and the FILETIME → Unix-ms conversion,
are a pure, platform-independent unit tested on every platform.

**Tech Stack:** C++23, Win32 (`GetFileInformationByHandleEx` — `FileBasicInfo`, `FileStandardInfo`,
`FileAttributeTagInfo`, `FileIdInfo` —, `GetFileInformationByHandle`, `GetFileType`, `ReOpenFile`, `ReadFile`),
ntdll `NtCreateFile` through phase A's `openBeneath()`, Catch2 (`vtpty_test`, `vtextension_test`, `vthost_test`).

**Spec:** `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md` (phase 4, "C — Windows
`file-read`": "Follow the design comment in `BeneathRead_win32.cpp`"). Builds on
`2026-11-23-orchestrator-windows-directory-handles.md` (phase A, implemented) and, for Task 4 only,
`2026-11-23-orchestrator-windows-extension-roots.md` (phase A2).

## Depends on

- **Phase A, all three tasks** (landed: `235049318`, `04a9e29d4`, `7e8a22f1a`), **plus the working-tree change to
  `src/vtpty/Win32OpenBeneath.{hpp,cpp}` that is uncommitted at the time of writing** (`BeneathEntry::Last`, the
  `EntryRows` order `static_assert`s, `isOneComponent`, `SYNCHRONIZE` always added). Tasks 1–3 add rows to that
  table: commit it first.
- **Phase A2 Tasks 1, 2, 5, 6, 7** before Task 4 only: `vtextension_test` must build on Windows (A2 Task 1), the
  opener must report its `PathStyle` and roots resolve in it (A2 Tasks 2, 5, 6), and `FileReadService` must split,
  compare and fence in the opener's grammar (A2 Task 7). **This plan does not touch `FileReadService.cpp`** — A2
  Task 7 owns it; Task 4 here only adds Windows tests through the native reader.
- Phase B (process runner) and phase D (pane-open) are independent of this plan.

## Global Constraints

- Project rules in `AGENT.md`: C++23, `std::expected`, `enum class` over `bool` (new enums `: uint8_t`, zero the
  negative/default case), Doxygen on every new public declaration, `.hpp` + `#pragma once`, no C-style loops
  (`std::views::iota`), no `NOLINT`, zero warnings under `-Werror` and clang-tidy, data-driven tables over scattered
  `if`s (error mappings, entry rows, accepted reparse tags).
- Never edit `vendor/core-cpp`. Reuse `core::pathFromUtf8` (UTF-8 → UTF-16; failure = not UTF-8).
- **POSIX behaviour is unchanged.** `BeneathRead_unix.cpp` and the `#ifndef _WIN32` half of `BeneathRead_test.cpp`
  are not edited. The only shared-header changes are one defaulted field (`FileIdentity::inodeHigh`, always 0 on
  POSIX) and Doxygen text. Linux CI is the gate for the POSIX half.
- **Every handle the reader makes is non-inheritable** (phase A's `openBeneath` sets no `OBJ_INHERIT`;
  `ReOpenFile` gets no `SECURITY_ATTRIBUTES`) and is opened `FILE_SHARE_READ | FILE_SHARE_WRITE |
  FILE_SHARE_DELETE`: a read must never stop an agent from rewriting, renaming or deleting the file it reads.
- No handle outlives the call: every handle is a `UniqueHandle` local to `read()`/`identityOf()`.
- Do not edit `docs/extensions.md` or the roadmap file (uncommitted user work); doc text goes in "Docs for
  phase E" below.
- Do not run two builds in one tree at once. Builds take minutes: use a 10-minute timeout. Commands:
  - `cmake --build --preset clangcl-release --target vtpty_test` → `out/build/clangcl-release/bin/vtpty_test.exe`
  - `cmake --build --preset clangcl-release --target vtextension_test` → `out/build/clangcl-release/bin/vtextension_test.exe`
  - `cmake --build --preset clangcl-release --target vthost_test` → `out/build/clangcl-release/bin/vthost_test.exe`
- Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

## Decisions

1. **A Windows seam of its own, and a Windows copy of the walk, rather than generalising `IBeneathSystem`.**
   `IBeneathSystem` speaks `int` descriptors, `OwnedFd` and errno; widening it to native handles would edit the
   POSIX reader and its tests, which the constraint above forbids. `IWin32BeneathSystem` lives in a Windows-only
   header (`Win32BeneathRead.hpp`, as `Win32OpenBeneath.hpp` is), and the Windows reader mirrors
   `NativeBeneathReader` step for step (same `Step` split, same order of checks, same error vocabulary) so the two
   read side by side. Unifying them behind a handle-type template is a possible later refactor once both are
   tested, not this phase.
2. **The last component is looked at, then re-opened as the same object — never opened by name twice.** POSIX
   looks (`fstatat`) and then opens by name, because opening a FIFO or a device can block or act; the name can
   change between the two, and `fstat` on the descriptor is the authority. On Windows the look is an open with
   `FILE_READ_ATTRIBUTES` only (`BeneathEntry::Entry`, `FILE_OPEN_REPARSE_POINT`); only an entry classified
   `Regular` is then given read access by `ReOpenFile(handle, FILE_READ_DATA | SYNCHRONIZE, ShareAll,
   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_SEQUENTIAL_SCAN)`, which reopens the object
   already held, so there is no window between look and read. The re-opened handle is described again and that
   description is the authority (links, volume, kind), as `fstat` is on POSIX.
3. **What an entry is** (pure rule, `windowsFileKindOf`, Task 1):
   - a reparse point whose tag is a name surrogate (junction, symlink, mount point) → `FileKind::Symlink` →
     `Escapes`, at every component;
   - a reparse point whose tag is not in `AcceptedStorageReparseTags` (dedup, WOF, the cloud-files family) →
     `Other` → `NotRegular` (an `AF_UNIX` socket, an App Execution Alias, an unknown filter's tag — as POSIX
     answers a socket `ENXIO` → `NotRegular`);
   - a file carrying `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` or `FILE_ATTRIBUTE_OFFLINE` (a cloud placeholder whose
     data is not on disk) → `Other` → `NotRegular`: **the reader never asks a network for a file's bytes**, the
     analogue of POSIX never opening a FIFO. `FILE_FLAG_OPEN_NO_RECALL` on the re-open backs this up;
   - `FILE_ATTRIBUTE_DIRECTORY` → `Directory`; `GetFileType() != FILE_TYPE_DISK` → `Other`; else `Regular`.
   Directories with storage tags (OneDrive-redirected folders, dedup) are walked, as phase A Decision 2 accepts
   them for `createBeneath`.
4. **Identity is `FILE_ID_INFO`: the 64-bit volume serial and the 128-bit file id.** `FileIdentity` gains one
   defaulted field, `inodeHigh` (the upper 64 bits; zero on NTFS, used by ReFS — Dev Drive is ReFS). Where
   `FileIdInfo` is not supported (FAT, exFAT, some redirectors), `GetFileInformationByHandle`'s 32-bit serial and
   64-bit index are used. **A file id of zero is "unknown" and the entry is `Unreadable`**, at every step: some SMB
   servers answer zero for every file, and a zero identity would match every fence (or none); refusing is the
   fail-closed choice — `file-read` does not work on such shares.
5. **Links:** `FILE_STANDARD_INFO::NumberOfLinks != 1` on the file → `Escapes` (hard links need no privilege on
   NTFS). A delete-pending entry is reported with `links = 0`, so "a root removed while it is held" reads
   `DirectoryMissing` through the same `links == 0` test POSIX uses. `ERROR_ACCESS_DENIED` while opening beneath a
   directory that is itself delete-pending (`STATUS_DELETE_PENDING`) is `DirectoryMissing`, as phase A's
   `createBeneath` maps it to `Missing`.
6. **Names.** Every component must pass `isPlainWindowsName()` and convert to UTF-16, else `Escapes` before
   anything is opened (`""`, `.`, `..`, `a\b`, `a/b`, `STATUS.md:x`, `x::$DATA`, `CON`, `nul.txt`, `x.`, `x `,
   `<>"|?*`, control characters, non-UTF-8). `FileReadService` refuses most of these earlier as `BadArgument` (A2
   Task 7); the reader refuses them itself because it is a public `vtpty` interface. **Case variants and 8.3
   short names are opened**: a relative `NtCreateFile` resolves both, and refusing `~1`-shaped names would refuse
   real files. Confinement rests on handles, and fencing on identity, never on spelling — Task 3 tests a fenced
   directory reached by another case and by its short name.
7. **Volume:** every directory walked and the file must report the root's volume serial, else `Escapes`. Every
   way onto another volume (mount point, junction, symlink) is already a name surrogate refused by Decision 3;
   the comparison is the defence in depth POSIX's `st_dev` check is.
8. **`makeBeneathSystem()` and `makeBeneathReader(IBeneathSystem&)` stay `Unsupported` on Windows**: they take
   the POSIX seam, which Windows cannot serve. `nativeBeneathReader()` becomes the working Windows reader, and
   `makeWin32BeneathReader(IWin32BeneathSystem&)` is the test-injection entry point. Only tests call
   `makeBeneathReader`, all of them POSIX-only.
9. **Reads are synchronous and bounded by bytes, not by time.** At most `maxBytes + 1` bytes are read in 16 KiB
   chunks; more than `maxBytes` is `TooLarge` whatever `EndOfFile` said. A stalled SMB server can still hold a
   read, exactly as a stalled NFS mount can on POSIX (residual, documented at the declaration).

## Review Focus

Security first. Invariant: **nothing outside the held root, and nothing inside a fenced directory, is ever read**,
whatever the names, links or timing.

- **Link-following at any component.** A junction (any user can make one) or a directory symlink as a middle
  component, and a junction, file symlink or directory symlink as the last one → `Escapes`, nothing behind it
  opened. Every open carries `FILE_OPEN_REPARSE_POINT`; check no code path opens without it (`EntryRows`, the
  `ReOpenFile` flags). Task 2 (seam), Task 3 (reader).
- **TOCTOU.** No name is resolved twice: directories are opened relative to the handle above (never from the
  root's path), and the file is re-opened by handle (Decision 2). A middle directory renamed after it was opened
  keeps being walked in place. Task 3, "a directory renamed during the walk is still the one walked".
- **8.3 names, case, ADS, device names.** Task 1 (rule table) and Task 3: `STATUS.md:x`, `x::$DATA`, `CON`,
  `nul.txt`, `COM1`, `trail.`, `trail ` → `Escapes`; `status.MD` reads `STATUS.md`; a fenced `Main` reached as
  `main` and as its short name → `Fenced`.
- **Second names.** A hard link to a file outside the root → `Escapes` (Task 3).
- **Oversized files.** `maxBytes + 1` is the most read; a file larger than `EndOfFile` said (rewritten status) →
  `TooLarge`; exactly `maxBytes` → read (Task 3).
- **Non-files.** A directory, an `AF_UNIX` socket, a dehydrated cloud placeholder, an unknown reparse tag →
  `NotRegular`, and the placeholder's data is never fetched (Task 1 rules; Task 3 by injected attributes).
- **Handle leaks.** Every handle is a local `UniqueHandle`; none is inheritable; a read leaves the file deletable
  (Task 3, "a read holds nothing open afterwards"); share mode never blocks a writer (Task 3, "a file open for
  writing elsewhere is read").
- **Unknown identities.** A zero file id → `Unreadable`, never a match (Task 3 by injection).
- **Unbacked root** (`DirectoryHandle::unbacked`) → `Unreadable`, never an NT open relative to a null root
  (phase A's `openBeneath` refuses it too). Task 3.

---

### Task 1: What a Windows entry is, as a pure rule

**Files:**
- Create: `src/vtpty/WindowsBeneathRules.hpp`, `src/vtpty/WindowsBeneathRules.cpp`,
  `src/vtpty/WindowsBeneathRules_test.cpp`
- Modify: `src/vtpty/CMakeLists.txt` (`WindowsBeneathRules.cpp` in `vtpty_SOURCES` beside `WindowsPathRules.cpp`,
  the header in `vtpty_HEADERS`, the test in `vtpty_test`'s unconditional list)

**Interfaces** (namespace `vtpty`; no `Windows.h` — the Win32 values are spelled as documented numbers here, and
Task 2 `static_assert`s each against the SDK macro of the same meaning):

```cpp
/// The FILE_ATTRIBUTE_* bits the rule reads, as Win32 documents their values.
namespace WindowsAttribute
{
    inline constexpr uint32_t Directory = 0x0000'0010;          ///< FILE_ATTRIBUTE_DIRECTORY
    inline constexpr uint32_t ReparsePoint = 0x0000'0400;       ///< FILE_ATTRIBUTE_REPARSE_POINT
    inline constexpr uint32_t Offline = 0x0000'1000;            ///< FILE_ATTRIBUTE_OFFLINE
    inline constexpr uint32_t RecallOnDataAccess = 0x0040'0000; ///< FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
} // namespace WindowsAttribute

/// IsReparseTagNameSurrogate()'s bit: junctions, symlinks, mount points.
inline constexpr uint32_t WindowsNameSurrogateBit = 0x2000'0000;

/// A reparse tag that marks how a real file or directory is stored, not where it leads, and the mask of the
/// bits that may vary within its family.
struct StorageReparseTag
{
    uint32_t tag;  ///< The tag, its variable bits zero.
    uint32_t mask; ///< The bits ignored when comparing (IO_REPARSE_TAG_CLOUD_MASK for the cloud family).
};

/// The storage tags a read accepts: one row per family, a new family a new row.
inline constexpr auto AcceptedStorageReparseTags = std::to_array<StorageReparseTag>({
    { .tag = 0x8000'0013, .mask = 0 },          // IO_REPARSE_TAG_DEDUP
    { .tag = 0x8000'0017, .mask = 0 },          // IO_REPARSE_TAG_WOF
    { .tag = 0x9000'001A, .mask = 0x0000'F000 }, // IO_REPARSE_TAG_CLOUD .. CLOUD_F
});

/// What GetFileType() says a handle is, as far as a read cares.
enum class WindowsHandleType : uint8_t
{
    Other = 0, ///< A pipe, a character device, unknown.
    Disk,      ///< FILE_TYPE_DISK.
};

/// What a Windows handle tells of the entry it opened.
struct WindowsEntryFacts
{
    uint32_t attributes = 0;                              ///< FILE_ATTRIBUTE_* bits.
    uint32_t reparseTag = 0;                              ///< Its tag, where attributes holds ReparsePoint.
    WindowsHandleType handleType = WindowsHandleType::Other; ///< GetFileType()'s answer.
};

/// @param facts What a handle tells of its entry.
/// @return What the entry is to a read beneath a directory (Decision 3): Symlink for a name surrogate;
///         Other for a reparse tag not in AcceptedStorageReparseTags, a non-disk handle, or data not on disk
///         (Offline, RecallOnDataAccess) on a non-directory; Directory; else Regular.
[[nodiscard]] FileKind windowsFileKindOf(WindowsEntryFacts facts) noexcept;

/// @param fileTime A FILETIME as one 64-bit count of 100 ns since 1601-01-01 UTC.
/// @return Milliseconds since the Unix epoch; 0 for any time before it.
[[nodiscard]] uint64_t unixMsOfFileTime(uint64_t fileTime) noexcept;
```

`FileKind` comes from `BeneathRead.hpp` (include it; it is platform-independent).

- [ ] **Step 1: Write the failing tests** in `WindowsBeneathRules_test.cpp`, tag `[vtpty][windowsbeneathrules]`:

```cpp
TEST_CASE("windowsFileKindOf: plain files and directories", "[vtpty][windowsbeneathrules]")
{
    using vtpty::FileKind; using vtpty::WindowsHandleType; namespace A = vtpty::WindowsAttribute;
    CHECK(vtpty::windowsFileKindOf({ .attributes = 0x20 /*ARCHIVE*/, .reparseTag = 0, .handleType = WindowsHandleType::Disk }) == FileKind::Regular);
    CHECK(vtpty::windowsFileKindOf({ .attributes = A::Directory, .reparseTag = 0, .handleType = WindowsHandleType::Disk }) == FileKind::Directory);
    CHECK(vtpty::windowsFileKindOf({ .attributes = 0, .reparseTag = 0, .handleType = WindowsHandleType::Other }) == FileKind::Other);
}

TEST_CASE("windowsFileKindOf: a name surrogate is a link, file or directory", "[vtpty][windowsbeneathrules]")
{   // tags 0xA000000C (SYMLINK), 0xA0000003 (MOUNT_POINT), with and without A::Directory → FileKind::Symlink
}

TEST_CASE("windowsFileKindOf: storage tags are what they store", "[vtpty][windowsbeneathrules]")
{   // DEDUP, WOF, CLOUD (0x9000001A), CLOUD_3 (0x9000301A), CLOUD_F (0x9000F01A) on a file → Regular,
    // on a directory → Directory
}

TEST_CASE("windowsFileKindOf: any other reparse tag is something else", "[vtpty][windowsbeneathrules]")
{   // AF_UNIX 0x80000023, APPEXECLINK 0x8000001B, 0x12345678, CLOUD with a stray low bit 0x9000001B → Other
    // (also with A::Directory: a non-surrogate unknown tag on a directory is Other, not walked)
}

TEST_CASE("windowsFileKindOf: a file whose data is not on disk is something else", "[vtpty][windowsbeneathrules]")
{   // A::RecallOnDataAccess, A::Offline, each with the CLOUD tag and without → Other;
    // A::Directory | A::RecallOnDataAccess → Directory (a placeholder directory is walked, Decision 3)
}

TEST_CASE("unixMsOfFileTime", "[vtpty][windowsbeneathrules]")
{
    CHECK(vtpty::unixMsOfFileTime(116'444'736'000'000'000ULL) == 0);              // 1970-01-01
    CHECK(vtpty::unixMsOfFileTime(116'444'736'000'000'000ULL + 12'345'678) == 1234); // 100 ns units, truncated
    CHECK(vtpty::unixMsOfFileTime(0) == 0);                                          // before the epoch
    CHECK(vtpty::unixMsOfFileTime(133'000'000'000'000'000ULL) == 1'655'526'400'000ULL);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test`
Expected: compile error, `'vtpty/WindowsBeneathRules.hpp' file not found`.

- [ ] **Step 3: Implement.** `windowsFileKindOf` in the order of Decision 3; the storage-tag test is
  `std::ranges::any_of(AcceptedStorageReparseTags, [&](auto row) { return (tag & ~row.mask) == row.tag; })`.
  `unixMsOfFileTime`: `constexpr auto UnixEpochFileTime = uint64_t { 116'444'736'000'000'000 };`.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[windowsbeneathrules]"`
Expected: `All tests passed (… assertions in 6 test cases)`.

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/WindowsBeneathRules.hpp src/vtpty/WindowsBeneathRules.cpp src/vtpty/WindowsBeneathRules_test.cpp src/vtpty/CMakeLists.txt
git commit -m "feat(vtpty): what a Windows entry is to a read, as a pure rule

A name-surrogate reparse point is a link; a storage tag (dedup, WOF,
cloud files) is what it stores; any other tag, a non-disk handle, or a
placeholder whose data is not on disk is something a read refuses.
Tested on every platform.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The Windows seam — open, re-open, describe and read beneath a handle

**Files:**
- Modify: `src/vtpty/Win32OpenBeneath.hpp`, `src/vtpty/Win32OpenBeneath.cpp` (two `BeneathEntry` enumerators and
  their `EntryRows`; `Last` moves to the new last one)
- Create: `src/vtpty/Win32BeneathRead.hpp` (whole header `#ifdef _WIN32`, includes `BeneathRead.hpp`,
  `Win32OpenBeneath.hpp`), `src/vtpty/Win32BeneathSystem.cpp`
- Modify: `src/vtpty/BeneathRead.hpp` (`FileIdentity::inodeHigh`; Doxygen of `FileIdentity`, `FileStatus`)
- Modify: `src/vtpty/CMakeLists.txt` (`Win32BeneathSystem.cpp` in the Windows source block beside
  `Win32OpenBeneath.cpp`, the header in `vtpty_HEADERS` there)
- Create (unless phase D Task 1 already did): `src/vtpty/test/Win32Junction.hpp` (header-only, `#ifdef _WIN32`):
  `MountPointHeader` and `createJunction()` moved verbatim out of `DirectoryHandle_test.cpp`'s anonymous namespace
  into `namespace vtpty::testing`, `inline`. Whichever of C and D lands first moves it; the other only includes it.
- Modify: `src/vtpty/DirectoryHandle_test.cpp` (include `vtpty/test/Win32Junction.hpp`, delete its own copy)
- Create: `src/vtpty/Win32BeneathSystem_test.cpp` (whole file `#ifdef _WIN32`; in the `if(WIN32)` test sources)

**Interfaces:**

```cpp
// Win32OpenBeneath.hpp — BeneathEntry gains, in this order after DirectoryCreatedIfMissing:
    Directory, ///< An existing directory, to walk through: FILE_OPEN, FILE_DIRECTORY_FILE.
    Entry,     ///< Any existing entry, looked at: FILE_OPEN, neither FILE_DIRECTORY_FILE nor FILE_NON_DIRECTORY_FILE.
    Last = Entry,
// Every row keeps FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT. No FILE_NON_DIRECTORY_FILE anywhere:
// its STATUS_FILE_IS_A_DIRECTORY maps to ERROR_ACCESS_DENIED and could not be told from a real denial.

// BeneathRead.hpp — FileIdentity gains (POSIX never sets it):
    uint64_t inodeHigh = 0; ///< The upper half of a 128-bit file id (Windows ReFS); 0 on POSIX and NTFS.

// Win32BeneathRead.hpp (namespace vtpty)
/// The Win32 calls a read beneath a directory makes on Windows: the seam a test injects an error, an identity, a
/// size or attributes through. The Windows counterpart of IBeneathSystem, which speaks POSIX descriptors.
class IWin32BeneathSystem
{
  public:
    // non-copyable, non-movable, virtual destructor (as IBeneathSystem)

    /// Opens one entry beneath @p parent through openBeneath(), following no reparse point.
    /// @param parent An open directory.
    /// @param name One component, UTF-16, already checked by isPlainWindowsName().
    /// @param entry BeneathEntry::Directory (FILE_TRAVERSE | FILE_READ_ATTRIBUTES) or BeneathEntry::Entry
    ///              (FILE_READ_ATTRIBUTES): the access is the entry's, from one table.
    /// @return The handle; or the Win32 error.
    [[nodiscard]] virtual std::expected<UniqueHandle, DWORD> open(HANDLE parent, std::wstring_view name,
                                                                  BeneathEntry entry) = 0;

    /// Re-opens the object @p looked holds for reading its data (ReOpenFile, Decision 2): no name is resolved.
    /// @return A synchronous handle with FILE_READ_DATA; or the Win32 error.
    [[nodiscard]] virtual std::expected<UniqueHandle, DWORD> reopenForRead(HANDLE looked) = 0;

    /// @return What @p handle is: kind (windowsFileKindOf), identity (FILE_ID_INFO, else
    ///         GetFileInformationByHandle), links (0 when delete-pending), size (EndOfFile), modification time;
    ///         or the Win32 error of the first query that failed.
    [[nodiscard]] virtual std::expected<FileStatus, DWORD> status(HANDLE handle) = 0;

    /// Reads up to @p into.size() bytes at the handle's position.
    /// @return How many were read, 0 at the end; or the Win32 error.
    [[nodiscard]] virtual std::expected<std::size_t, DWORD> read(HANDLE handle, std::span<char> into) = 0;
};

/// @return This machine's calls.
[[nodiscard]] IWin32BeneathSystem& nativeWin32BeneathSystem() noexcept;
```

`Win32BeneathSystem.cpp` `static_assert`s every `WindowsAttribute`/tag constant of Task 1 equal to the SDK macro
(`FILE_ATTRIBUTE_DIRECTORY`, `IO_REPARSE_TAG_DEDUP`, `IO_REPARSE_TAG_CLOUD_MASK`, …). The access table is one
`constexpr std::array` indexed by `BeneathEntry` (a `DirectoryCreatedIfMissing` row of `HeldDirectoryAccess` so the
index lines up, with the same order `static_assert` as `EntryRows`).

- [ ] **Step 1: Write the failing tests** in `Win32BeneathSystem_test.cpp`, tag `[vtpty][beneath]`, fixture: a
  `ScopedTempDir`, `root` opened by `nativeDirectoryOpener()`, helpers `openIn(parent, name, entry)`:

```cpp
TEST_CASE("Win32BeneathSystem: a file is regular, with one link, on the root's volume", "[vtpty][beneath]")
{   // root\f holding "abc"; open(root, L"f", Entry) → status: Regular, links 1, sizeBytes 3,
    // identity.device == status(root).identity.device, identity.inode != 0, modifiedUnixMs within 1 min of now
}
TEST_CASE("Win32BeneathSystem: one file opened by two spellings is one identity", "[vtpty][beneath]")
{   // open(root, L"f") and open(root, L"F") → equal identity; a second file → different identity
}
TEST_CASE("Win32BeneathSystem: a junction is opened itself, as a link", "[vtpty][beneath]")
{   // createJunction(root\j, outside); open(root, L"j", Directory) succeeds and status → Symlink;
    // open(root, L"j", Entry) → Symlink
}
TEST_CASE("Win32BeneathSystem: a directory open of a file is ERROR_DIRECTORY", "[vtpty][beneath]")
{   // open(root, L"f", Directory).error() == ERROR_DIRECTORY
}
TEST_CASE("Win32BeneathSystem: a missing entry is ERROR_FILE_NOT_FOUND", "[vtpty][beneath]")
{   // both entries
}
TEST_CASE("Win32BeneathSystem: a hard link counts its names", "[vtpty][beneath]")
{   // CreateHardLinkW(root\h, outside\private) → status(open(root, L"h", Entry)).links == 2
}
TEST_CASE("Win32BeneathSystem: a removed held directory has no links", "[vtpty][beneath]")
{   // open root\wt as a DirectoryHandle; remove it; status(nativeHandle()).links == 0
}
TEST_CASE("Win32BeneathSystem: a re-opened file reads its bytes, and is the same file", "[vtpty][beneath]")
{   // looked = open(root, L"f", Entry); data = reopenForRead(looked); status(data).identity == status(looked)
    // .identity; read(data, 16 bytes) == 3 → "abc"; next read == 0; std::filesystem::remove(root\f) succeeds
    // while `data` is still open (FILE_SHARE_DELETE)
}
TEST_CASE("Win32BeneathSystem: no handle it makes is inheritable", "[vtpty][beneath]")
{   // GetHandleInformation on open(...) and reopenForRead(...) handles: HANDLE_FLAG_INHERIT clear
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test`
Expected: compile error, `'vtpty/Win32BeneathRead.hpp' file not found`.

- [ ] **Step 3: Implement** the two rows, the seam, `nativeWin32BeneathSystem()` (a function-local static), the
  junction helper move. `status()`: `FileAttributeTagInfo`, `FileStandardInfo`, `FileBasicInfo`
  (`LastWriteTime` → `unixMsOfFileTime`), `FileIdInfo` with the `GetFileInformationByHandle` fallback on
  `ERROR_INVALID_PARAMETER`/`ERROR_NOT_SUPPORTED` only, `GetFileType`. `reopenForRead`: `ReOpenFile` returns
  `INVALID_HANDLE_VALUE` on failure, not null — map it. If the re-open test shows `ReOpenFile` refusing
  `FILE_READ_DATA` that the attributes-only look did not hold, do not widen silently: give `BeneathEntry::Entry`
  `FILE_READ_DATA | FILE_READ_ATTRIBUTES` (one handle both looks and reads — still one name lookup, and opening a
  regular file's handle for reading acts on nothing; the `FILE_FLAG_OPEN_NO_RECALL` guarantee then rests on
  Decision 3's attribute check alone), make `reopenForRead` a duplicate, record why at the access table, and say so
  in the commit body.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "Win32BeneathSystem*" && out/build/clangcl-release/bin/vtpty_test.exe "DirectoryHandle*"`
Expected: `All tests passed` for both (9 new cases; every `DirectoryHandle` case as before the move).

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/Win32OpenBeneath.hpp src/vtpty/Win32OpenBeneath.cpp src/vtpty/Win32BeneathRead.hpp src/vtpty/Win32BeneathSystem.cpp src/vtpty/Win32BeneathSystem_test.cpp src/vtpty/BeneathRead.hpp src/vtpty/test/Win32Junction.hpp src/vtpty/DirectoryHandle_test.cpp src/vtpty/CMakeLists.txt
git commit -m "feat(vtpty): the Win32 calls a read beneath a held directory makes

Entries are opened relative to the handle above them as themselves (a
junction is opened, not followed), a looked-at file is re-opened for
reading as the same object, and each handle is described as the read
needs it: kind, 128-bit identity on its volume, links, size, mtime.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: The Windows reader

**Files:**
- Modify: `src/vtpty/BeneathRead_win32.cpp` (rewrite: keep `UnsupportedBeneathSystem` for `makeBeneathSystem` /
  `nativeBeneathSystem`, add the Windows reader; replace the design comment by Doxygen on the reader)
- Modify: `src/vtpty/Win32BeneathRead.hpp` (add `makeWin32BeneathReader`)
- Modify: `src/vtpty/BeneathRead.hpp` (Doxygen only: `makeBeneathSystem`, `makeBeneathReader`,
  `nativeBeneathReader`, `IBeneathReader::read`'s component rule and its stalled-share residual, Decision 9)
- Modify: `src/vtpty/BeneathRead_test.cpp` (delete the `#else` "Windows reads nothing beneath a directory" case
  and its includes; the file ends `#endif` after the POSIX block)
- Create: `src/vtpty/BeneathRead_win32_test.cpp` (whole file `#ifdef _WIN32`; in the `if(WIN32)` test sources)

**Interfaces:**

```cpp
// Win32BeneathRead.hpp
/// @param system The Win32 calls the reader makes; outlives the reader.
/// @return A reader over this machine's filesystem through @p system, with IBeneathReader's contract on Windows.
[[nodiscard]] std::unique_ptr<IBeneathReader> makeWin32BeneathReader(IWin32BeneathSystem& system);
```

**The reader** (anonymous namespace in `BeneathRead_win32.cpp`; names indicative):

```cpp
enum class Step : uint8_t { Directory, File };            // as POSIX

struct Win32ErrorRow { Step step; DWORD error; BeneathReadError meaning; };
/// Every Win32 error the walk answers by name; any other is Unreadable, never DirectoryMissing.
constexpr auto Win32ErrorRows = std::to_array<Win32ErrorRow>({
    { Step::Directory, ERROR_FILE_NOT_FOUND, BeneathReadError::DirectoryMissing },
    { Step::Directory, ERROR_PATH_NOT_FOUND, BeneathReadError::DirectoryMissing },
    { Step::Directory, ERROR_DIRECTORY,      BeneathReadError::NotRegular },   // a file where a directory is
    { Step::Directory, ERROR_ACCESS_DENIED,  BeneathReadError::Unreadable },
    { Step::File,      ERROR_FILE_NOT_FOUND, BeneathReadError::NotRegular },
    { Step::File,      ERROR_PATH_NOT_FOUND, BeneathReadError::NotRegular },
    { Step::File,      ERROR_ACCESS_DENIED,  BeneathReadError::Unreadable },
});
```

`read(root, components, maxBytes, fenced)`, in order (mirrors `NativeBeneathReader::read`):
1. `components` empty, or any not `isPlainWindowsName` or not UTF-8 → `Escapes`.
2. `root.nativeHandle() == nullptr` → `Unreadable`. `rootStatus = status(root)`: failure → `Unreadable`;
   `links == 0` → `DirectoryMissing`; zero file id → `Unreadable`.
3. `walk()` the directories: `open(parent, name, Directory)`; failure → `openFailure(Step::Directory, …)`, which
   on `ERROR_DIRECTORY` looks again with `Entry` and answers `Escapes` for a `Symlink` (a file symlink in a
   directory's place, as POSIX's `ENOTDIR` re-look), and on `ERROR_ACCESS_DENIED` answers `DirectoryMissing`
   when the parent's status says `links == 0`; then `status`: failure/zero id → `Unreadable`, `Symlink` →
   `Escapes`, volume ≠ root's → `Escapes`, in `fenced` → `Fenced`.
4. Last component: `open(directory, name, Entry)` (failure → `openFailure(Step::File, …)`); `status` of it:
   `Symlink` → `Escapes`; anything but `Regular` → `NotRegular`.
5. `reopenForRead(looked)` (failure → `Unreadable`); `status` of the re-opened handle is the authority: not
   `Regular` → `NotRegular`; `links != 1`, volume ≠ root's, zero id → `Escapes`/`Escapes`/`Unreadable`.
6. `readUpTo(handle, sizeHint, maxBytes)`: 16 KiB chunks, at most `maxBytes + 1` bytes, `TooLarge` past
   `maxBytes`; a failed `read` → `Unreadable`.

`identityOf(root, components)`: steps 1 (empty allowed) – 3, then the last directory's (or the root's) identity.

**Tests** — `BeneathRead_win32_test.cpp`, tag `[vtpty][beneath]`, test names the POSIX ones verbatim where they
port. Fixture `Tree` as POSIX's (root and outside beside it, `outside\private` = "secret", `Limit` 64 KiB,
`open()`, `read()`, `readWith()`), reading through `makeWin32BeneathReader(nativeWin32BeneathSystem())`, plus a
`ForwardingWin32System` (as POSIX's `ForwardingSystem`: `failOpen(name) -> std::optional<DWORD>`,
`rewriteStatus(name, FileStatus&)`, names tracked by the handle value they were opened as, the re-open inheriting
its looked-at name).

```cpp
TEST_CASE("BeneathRead: a regular file is read with its mtime", "[vtpty][beneath]")
{   // root\wt\STATUS.md "# board\n"; read({"wt","STATUS.md"}) → bytes, modifiedUnixMs within 1 min of now;
    // also through nativeBeneathReader() — the production instance is the Windows reader
}
TEST_CASE("BeneathRead: an empty file and one of exactly the limit are read", "[vtpty][beneath]")
TEST_CASE("BeneathRead: a junction component escapes", "[vtpty][beneath]")
{   // createJunction(root\link, outside); read({"link","private"}) == Escapes
}
TEST_CASE("BeneathRead: a junction as the file escapes", "[vtpty][beneath]")
{   // createJunction(root\link, outside); read({"link"}) == Escapes
}
TEST_CASE("BeneathRead: a symlink component or file escapes", "[vtpty][beneath]")
{   // create_directory_symlink(outside, root\dlink) and create_symlink(outside\private, root\flink);
    // SKIP("no symlink privilege") when either fails; read({"dlink","private"}), read({"flink"}) == Escapes
}
TEST_CASE("BeneathRead: a hard link escapes", "[vtpty][beneath]")
{   // CreateHardLinkW(root\hard, outside\private) → read({"hard"}) == Escapes
}
TEST_CASE("BeneathRead: a file on another volume escapes", "[vtpty][beneath]")
{   // ForwardingWin32System rewrites identity.device of "file" (and, second section, of directory "wt") → Escapes
}
TEST_CASE("BeneathRead: a fenced directory is not entered, however it is spelled", "[vtpty][beneath]")
{   // root\Main\.git\config; fence = identityOf(root, {"Main"}); readWith({"Main",…}, {fence}) == Fenced;
    // readWith({"main",".git","config"}, {fence}) == Fenced (case); when GetShortPathNameW(root\Main-long-name)
    // yields a distinct short name, a directory "Main-long-name" fenced and read by its short name == Fenced
    // (else WARN "no 8.3 names on this volume"); a sibling "main-spelled" not fenced is read
}
TEST_CASE("BeneathRead: a directory's identity is reached as a read walks", "[vtpty][beneath]")
{   // identityOf(root, {"a","b"}) == identityOf(open(root\a\b), {}); via a junction → Escapes; "gone" →
    // DirectoryMissing
}
TEST_CASE("BeneathRead: a root removed while it is held is directory-missing", "[vtpty][beneath]")
TEST_CASE("BeneathRead: a missing directory is directory-missing, a missing file not-regular", "[vtpty][beneath]")
TEST_CASE("BeneathRead: a file where a directory should be is not-regular", "[vtpty][beneath]")
TEST_CASE("BeneathRead: a .git that became a directory is not-regular", "[vtpty][beneath]")
TEST_CASE("BeneathRead: a file over the limit is too-large", "[vtpty][beneath]")
TEST_CASE("BeneathRead: a file that grows past its size is too-large", "[vtpty][beneath]")
{   // 70 KiB file, rewriteStatus sizeBytes = 10 → TooLarge
}
TEST_CASE("BeneathRead: a component that is not one plain Windows name escapes", "[vtpty][beneath]")
{   // {}, {"wt","..","..","outside","private"}, {"..","outside","private"}, {".","wt","file"}, {"wt","","file"},
    // {"wt/file"}, {"wt\\file"}, {"C:\\Windows\\win.ini"}, {"wt","file:x"}, {"wt","file::$DATA"}, {"CON"},
    // {"wt","nul.txt"}, {"wt","file."}, {"wt","file "}, {"wt","fi\0le"sv}, {"\xff"} → Escapes;
    // the directory is unchanged and outside\private never opened (failOpen records every name asked: none of
    // these reaches the system)
}
TEST_CASE("BeneathRead: names compare as Windows compares them", "[vtpty][beneath]")
{   // root\wt\STATUS.md; read({"WT","status.MD"}) → the file's bytes
}
TEST_CASE("BeneathRead: a directory that may not be entered is unreadable", "[vtpty][beneath]")
{   // failOpen("closed") = ERROR_ACCESS_DENIED → Unreadable
}
TEST_CASE("BeneathRead: a file another process holds exclusively is unreadable", "[vtpty][beneath]")
{   // CreateFileW(root\locked, GENERIC_READ, 0 /*no sharing*/) held by the test → read({"locked"}) == Unreadable
}
TEST_CASE("BeneathRead: a file open for writing elsewhere is read", "[vtpty][beneath]")
{   // std::ofstream on root\STATUS.md kept open, flushed → read({"STATUS.md"}) → its bytes
}
TEST_CASE("BeneathRead: a read holds nothing open afterwards", "[vtpty][beneath]")
{   // after read({"wt","STATUS.md"}): std::filesystem::rename(root\wt, root\wt2) succeeds (NTFS refuses to
    // rename a directory while anything beneath it is open) and remove of the file succeeds
}
TEST_CASE("BeneathRead: what is not a file on disk is not-regular", "[vtpty][beneath]")
{   // rewriteStatus kind = Other for "placeholder" (as windowsFileKindOf answers a recall-on-data-access file)
    // → NotRegular, and reopenForRead was never called for it (the forwarding system counts calls)
}
TEST_CASE("BeneathRead: an unknown file identity is unreadable, never a match", "[vtpty][beneath]")
{   // rewriteStatus identity.inode = inodeHigh = 0 for "wt" → Unreadable even with an empty fence list, and
    // with a fence of {device, 0, 0}; for the root → Unreadable
}
TEST_CASE("BeneathRead: an unbacked root is unreadable", "[vtpty][beneath]")
{   // reader.read(DirectoryHandle::unbacked(root path), {"f"}, …) == Unreadable; identityOf likewise
}
TEST_CASE("BeneathRead: a directory renamed during the walk is still the one walked", "[vtpty][beneath]")
{   // root\a\b\f "inside"; ForwardingWin32System gains an afterOpen(name) hook: once "b" is opened, rename
    // root\a\b → root\a\b2 (allowed: NTFS refuses to rename only a directory with something open *beneath* it)
    // and create a new root\a\b holding f "planted" → the read answers "inside", never "planted"
}
```

Not ported, with the reason at the top of the file: "a named pipe answers not-regular without blocking" (no FIFOs
beneath an NTFS directory; the closest, an `AF_UNIX` socket, is a reparse tag covered by Task 1), "a real mount
crossing escapes" and the `openat2` probe of `/proc` (Linux-only; a Windows mount point is a name surrogate,
covered by the junction cases), "a symlink inside the root to a private file outside" (merged into the symlink
case).

- [ ] **Step 1: Write the tests above** (bodies spelled out from the comments; `CHECK`s exactly as stated), and
  delete the Windows "reads nothing" case from `BeneathRead_test.cpp`.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test`
Expected: compile error, `use of undeclared identifier 'makeWin32BeneathReader'`.

- [ ] **Step 3: Implement** the reader as specified; `nativeBeneathReader()` returns a function-local static
  reader over `nativeWin32BeneathSystem()`; `makeBeneathReader(IBeneathSystem&)` keeps returning the
  `Unsupported` reader (Decision 8), documented at the header.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[beneath]"`
Expected: `All tests passed` — Task 2's 9 cases plus these 25 (Task 1's carry `[windowsbeneathrules]`; the
deleted "reads nothing" case is gone).
Then the whole suite: `out/build/clangcl-release/bin/vtpty_test.exe` → `All tests passed`. Repeat `"[beneath]"`
10 times → 10 × `All tests passed` (no ordering dependence on temp-directory reuse).

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/BeneathRead_win32.cpp src/vtpty/Win32BeneathRead.hpp src/vtpty/BeneathRead.hpp src/vtpty/BeneathRead_test.cpp src/vtpty/BeneathRead_win32_test.cpp src/vtpty/CMakeLists.txt
git commit -m "feat(vtpty): read a file beneath a held directory on Windows

Each component is opened relative to the handle above it as itself, a
junction, symlink or mount point refused at every step; the file is
looked at, then re-opened as the same object, and read only when it is a
regular file with one name on the root's volume, at most maxBytes + 1
bytes. Fenced directories are recognised by their 128-bit identity,
whatever case or short name spells them.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `file-read` on Windows, end to end through the native reader

Requires phase A2 Tasks 1, 2, 5, 6, 7 (see "Depends on"). These tests should pass on first run once Task 3 and
A2 are in; if one fails, the fault is in Task 3 or in A2's Task 7 — fix it there, not in the test.

**Files:**
- Modify: `src/vtextension/FileReadService_test.cpp` (new `#ifdef _WIN32` block after the POSIX one)
- Modify: `src/vthost/DaemonWorldExtensions_test.cpp` (drop the `#ifndef _WIN32` guard around "the daemon world
  reads files through the native reader"; the link it plants becomes a helper)

- [ ] **Step 1: Write the tests.**

`FileReadService_test.cpp` (`#ifdef _WIN32`, tag `[vtextension][file-read]`; `RecordingOpener` moves out of the
POSIX block so both use it):

```cpp
TEST_CASE("FileReadService: a Windows worktree's STATUS.md is read through the native reader", "[vtextension][file-read]")
{   // wt = canonical(temp); wt\contour\r1\STATUS.md "# Board\n"; roots worktrees = { pathToUtf8(wt) };
    // service.read("worktrees", "contour/r1/STATUS.md") → its bytes, modifiedUnixMs != 0;
    // opener.opened == { pathToUtf8(wt) } (only the root is opened by path)
}
TEST_CASE("FileReadService: a nested root is fenced through the walk, not by path, on Windows", "[vtextension][file-read]")
{   // wt\Main\.git\config; repos root = wt\Main; read("worktrees", "Main/.git/config") == NotGranted and
    // read("worktrees", "main/.git/config") == NotGranted (another case: fenced by identity)
}
TEST_CASE("FileReadService: a junction inside a Windows root escapes", "[vtextension][file-read]")
{   // vtpty::testing::createJunction(wt\r1\out, outside); outside\secret;
    // read("worktrees", "r1/out/secret") == Escapes
}
TEST_CASE("FileReadService: a Windows stream or device name is a bad argument", "[vtextension][file-read]")
{   // "r1/STATUS.md:x", "r1/CON", "r1/x." → BadArgument (A2 Task 7's grammar, before the reader)
}
```

`DaemonWorldExtensions_test.cpp`: remove the guard and its comment; replace
`std::filesystem::create_symlink("/etc/passwd", work / "run-1" / "escape")` by a file-local helper
`plantEscape(work / "run-1" / "escape", outside)` that makes a symlink to `/etc/passwd` on POSIX and, on Windows,
`vtpty::testing::createJunction(link, <a temp directory beside work>)`; the guest's second path stays
`run-1/escape` on POSIX and becomes `run-1/escape/x` on Windows (a junction is a directory), still expected
`Escapes`. `configJson` spells the root with `work.generic_string()` (A2 accepts `C:/…` in a configured root and it
needs no JSON escaping). The expected `.git` contents stay byte-identical.

- [ ] **Step 2: Run**

Run: `cmake --build --preset clangcl-release --target vtextension_test && out/build/clangcl-release/bin/vtextension_test.exe "[file-read]"`
Expected: `All tests passed`.
Run: `cmake --build --preset clangcl-release --target vthost_test && out/build/clangcl-release/bin/vthost_test.exe "[file-read]"`
Expected: `All tests passed`. Then `vthost_test.exe "[daemon]"`: no case fails that did not fail at the roadmap's
recorded baseline.

- [ ] **Step 3: Commit**

```bash
git add src/vtextension/FileReadService_test.cpp src/vthost/DaemonWorldExtensions_test.cpp
git commit -m "test(vtextension): file-read on Windows through the native reader

A worktree's STATUS.md is read beneath the root's handle; a nested root
is fenced in any case; a junction out of the root escapes; the daemon
world's native-reader case now runs on Windows too.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Out of scope — gaps to raise

- **The walk is written twice** (POSIX and Windows, Decision 1). Once both suites are green on CI, a handle-typed
  template over one algorithm would remove the copy; it must not change the POSIX reader's observable behaviour.
- **SMB shares that answer zero file ids** cannot serve `file-read` (Decision 4). If users keep worktrees on such
  shares, a path-based fallback would need its own threat analysis.
- **`STATUS.md` replaced by rename while being opened** can transiently answer `Unreadable`
  (`STATUS_DELETE_PENDING` → `ERROR_ACCESS_DENIED`) where POSIX answers the old or new file; the orchestrator
  re-reads on its next refresh. Measure in phase E's end-to-end run whether Claude Code writes `STATUS.md` in
  place or by rename on Windows.

## Docs for phase E

- `docs/extensions.md`, `file-read`: "On Windows a file is read beneath the root's directory handle, one
  component at a time, as on Linux and macOS. A junction, symbolic link or mount point anywhere on the path, a
  file with a second name (a hard link), a file on another volume, a cloud-files placeholder whose contents are
  not on this disk (it is never downloaded), a socket or any other special entry is refused. Names compare
  ignoring case, and a short (8.3) name reaches the same file; a stream (`a:b`), a reserved device name (`CON`,
  `NUL`, …) or a name ending in a dot or space is a bad argument. A network share whose server does not report
  file identities cannot be read from."
- `docs/extensions.md`, Limitations: drop "file-read is not supported on Windows" (when phase E drops the Windows
  limitation as a whole).
- Roadmap table row C: "done" (phase E edits the roadmap; this plan does not).

## After the last task

- Run `/simplify` over the branch diff (AGENT.md workflow), then `clang-format` over touched files, and rebuild
  `vtpty_test`.
- Report in the change summary: performance — none on the terminal path; a `file-read` makes four
  `GetFileInformationByHandleEx` queries per component, at most once per Board refresh. Risk — Windows-only code
  behind an unchanged interface, one defaulted field in a shared struct (`FileIdentity::inodeHigh`, zero on
  POSIX). Coverage — the Windows reader by `BeneathRead_win32_test.cpp` and `Win32BeneathSystem_test.cpp`, the
  classification rule on every platform.
