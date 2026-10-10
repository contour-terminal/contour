# Semantic Blocks Implementation Plan

THIS IS A DRAFT DOCUMENT

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship every remaining #1010 item plus the agreed extras (per-block records, segmented gutter, scrollbar marks, sticky header, block actions, finish notifications, recent-commands picker, richer shell integration, settings-page coverage, daemon parity) in one PR that closes #1010.

**Architecture:** Rows carry a `CommandBlockId` and a 24-bit birth time in `Line`'s existing padding; a bounded, clock-injected `CommandBlockStore` owned by `Terminal` holds one record per block, fed by OSC 133 and OSC 3008 through `MarkArbiter`. Every decision (gutter layout, sticky header, scrollbar bucketing, sanitising, notification policy, picker ranking, click counting) is a pure unit with its own tests; `Terminal`, `TerminalSession`, the renderer and QML only wire them.

**Tech Stack:** C++23, CMake presets, Catch2 (libraries), Qt 6 / QML + `Qt6::Test` (GUI), yaml-cpp (config), the native `vthost` protocol, bash/zsh/fish/tcsh/PowerShell scripts.

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) (approved, commit `632cf37c`). Executors read the spec section a phase cites before starting it.

## How this plan is organised

One branch (`feature/1010-worktree-semantic-blocks`), one PR, ten phases in the spec's §15 order. Each phase
file is a self-contained plan; a phase starts only when the previous one builds clean and its tests
pass, and every phase ends with `/simplify` and an **xhigh** code review (see *Phase gate* below).

`file:L` and `file:L-L` references in every phase cite the tree at `632cf37c`; earlier tasks shift them.
Locate each edit by the quoted text, function or section it names and treat the number as a hint; where a
step says to delete or replace a line range, act on the named items, never on the numbers.

| # | Phase file | Spec | Depends on |
|---|---|---|---|
| 0 | [phase-00-setup.md](phase-00-setup.md) | — | — |
| 1 | [phase-01-foundation.md](phase-01-foundation.md) | §4 | 0 |
| 2 | [phase-02-daemon.md](phase-02-daemon.md) | §12 | 1 |
| 3 | [phase-03-shell-integration.md](phase-03-shell-integration.md) | §10 | 1 |
| 4 | [phase-04-gutter.md](phase-04-gutter.md) | §5 | 1, 3 |
| 5 | [phase-05-block-actions.md](phase-05-block-actions.md) | §7 | 1, 3, 4 |
| 6 | [phase-06-notifications.md](phase-06-notifications.md) | §8 | 1, 2, 3, 4 |
| 7 | [phase-07-sticky-header.md](phase-07-sticky-header.md) | §6.2, §6.3 | 1, 3, 4 |
| 8 | [phase-08-scrollbar-marks.md](phase-08-scrollbar-marks.md) | §6.1 | 1, 4, 5, 7 |
| 9 | [phase-09-picker.md](phase-09-picker.md) | §9 | 1, 2, 3, 5, 6 |
| 10 | [phase-10-settings-docs.md](phase-10-settings-docs.md) | §11, §15 | 1–9 |

## Global Constraints

Every task's requirements implicitly include all of these.

- C++23. Headers are `.hpp` with `#pragma once`. Range-based `for` only (no C-style loops); `std::views::iota` for index ranges.
- `[[nodiscard]]` where ignoring a result is a bug; `const` everywhere it holds; `auto` for readability.
- Doxygen (`///`, `@param`, `@return`) on every new public function, class, struct and member.
- Naming per `./.clang-tidy`: types `CamelCase`, everything else `camelBack`, non-public data members `_leadingUnderscore`.
- **No `bool` in new API surface** (parameters, returns that report success, members) — purpose-named `enum class ... : uint8_t` with the off/absent case at zero. Exceptions: YAML-facing `Config` fields (documented carve-out) and the `vtbackend::Settings` structs that mirror them one-to-one (`GutterSettings`, `StickyHeaderSettings`, as `Settings::foldMarkers` already does), predicates whose name asks the question (`isRunning()`), Qt virtuals/slots, and the predicate-snapshot fields of `contour::command::ContextMenuState` (Task 5.18).
- Fallible operations return `std::expected<T, E>` with a subsystem error enum; exceptions only for programmer errors.
- **Dependency injection** for time (`core::platform::IClock`, `core::platform::WallClockRef`), filesystem, processes, notifications. No new singletons, no static setters, no `std::chrono::*::now()` in new code outside the production clock adapters.
- **Configuration at construction**: new classes take their configuration in the constructor; no `init()`/setter phase.
- **Data-driven**: tables over switches wherever a sixth case would otherwise mean editing several places (gutter segments, glyphs, enum tokens, settings rows).
- **Zero warnings**: builds use `-Werror`/`/WX`; never silence a warning, never add `NOLINT` or `#pragma` mutes.
- `clang-format` every touched C++ file (`.clang-format` at the root).
- **Never edit `vendor/core-cpp`.**
- `Line` keeps `static_assert(sizeof(Line) == sizeof(LineSoA) + 32)`; new per-line data goes into the existing padding only (spec §5.4).
- No new third-party dependencies.
- Layering: `vtbackend` must not depend on anything above it; `contour/config`, `contour/command`, `contour/cli` stay Qt-free.
- Avoid Windows SDK macro names as identifiers: `small`, `near`, `far`, `min`, `max`, `interface`, `boolean`, `hyper`, `byte`, `ERROR`, `DELETE`, `IN`, `OUT`.
- Avoid `typos`-flagged spellings (e.g. write `invocable`, not `invokable`); `ctest -R check_spelling` must pass.
- Commit after every task. Messages: `<area>: <imperative summary>` in English, body optional, last line exactly:
  `Signed-off-by: Christian Parpart <christian@parpart.family>`
- Stage only the files a task names (`git add <paths>` or `git add -u`), never `git add -A`: the worktree may hold
  untracked local files that must never be committed.

## Build and test commands (Windows dev box)

The checkout has its own build tree; phase 0 configures it. All paths are relative to the root of the
checkout this branch (`feature/1010-worktree-semantic-blocks`) is checked out in — on the machine the
plan was written on, the worktree `D:\contour\.claude\worktrees\semantic-blocks-1010`.

| Purpose | Command |
|---|---|
| Configure (once) | `cmake --preset clangcl-debug` |
| Build one test target | `cmake --build --preset clangcl-debug --target vtbackend_test` |
| Run tests by tag / name | `out/build/clangcl-debug/bin/vtbackend_test.exe "[tag]"` or `"Test name"` |
| Full suite | `ctest --test-dir out/build/clangcl-debug --output-on-failure` |
| Spelling | `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure` |
| Format one file | `clang-format -i <file>` |

Run lines written `VAR=value <exe>` are Git Bash syntax. In PowerShell, set `$env:QT_QPA_PLATFORM = 'offscreen'` once and drop the prefix.

Test binaries live in `out/build/clangcl-debug/bin/`: `vtbackend_test`, `vtparser_test`, `vtrasterizer_test`,
`vthost_test`, `vtworkspace_test`, `contour_test` (Qt-free: `config/Actions_test.cpp`, `LayoutBuilder`,
`UiStyle`, `WindowControlStyle`, every `command/*_test.cpp`, every `cli/*_test.cpp`, `session/SearchStatus_test.cpp`,
`platform/FinishNotification_test.cpp` (phase 6) — `src/contour/CMakeLists.txt:805-820`), `contour_gui_test` (Qt; runs under `QT_QPA_PLATFORM=offscreen`, set
by ctest — **including `config/Config_test.cpp`** and every session, window, platform and display test,
`src/contour/CMakeLists.txt:598-656`). A new test file goes into whichever list its subject's library allows.

### On Linux or macOS

The phase files spell commands for the Windows box above. Elsewhere, translate them like this:

| Windows (as written) | Linux | macOS |
|---|---|---|
| `cmake --preset clangcl-debug` | `cmake --preset clang-asan` (Debug, ASan + UBSan; AGENT.md's default) | `cmake --preset appleclang-debug` |
| `cmake --build --preset clangcl-debug --target X` | `cmake --build --preset clang-asan --target X` | `cmake --build --preset appleclang-debug --target X` |
| `out/build/clangcl-debug/bin/X.exe` | `out/clang-asan/src/<module>/X` (e.g. `out/clang-asan/src/vtbackend/vtbackend_test`) | `out/appleclang-debug/src/<module>/X` |
| `ctest --test-dir out/build/clangcl-debug` | `ctest --preset=clang-asan` | `ctest --test-dir out/appleclang-debug` |

`CMAKE_RUNTIME_OUTPUT_DIRECTORY` is only set on Windows (`CMakeLists.txt:69-79`), so off Windows each test
binary sits in its module's build directory; when unsure, `find out/<preset> -name X -type f`. On Linux
the shell PTY tests (phase 3) actually run instead of skipping, clang-tidy can lint every file locally,
and the threading-sensitive phases (1, 2, 6) should also pass `ctest --preset=clang-tsan` (AGENT.md). The
Windows-only notes below do not apply there; the phase-3 ConPTY probe skips itself off Windows.

Notes from this machine (memory: windows-verify-tricks, windows-gui-test-run, wsl-clang-tidy-22-verify):
- The `clangcl-*` presets compile with MSVC `cl.exe` (Ninja). clang-tidy cannot lint `src/contour/**` on
  Windows; the CI clang-tidy job is the oracle for those files — hand-audit added lines for
  `misc-use-internal-linkage`, `misc-const-correctness`, `bugprone-implicit-widening-of-multiplication-result`.
- If `contour_gui_test` exits `0xC0000135`, the Qt runtime DLLs are not staged; run it with
  `$env:PATH = "C:\Qt\6.11.1\msvc2022_64\bin;" + $env:PATH` and
  `$env:QT_QPA_PLATFORM_PLUGIN_PATH = "C:\Qt\6.11.1\msvc2022_64\plugins\platforms"` (Qt path from `Qt6_DIR` in `CMakeCache.txt`).
- A full-tree build failing in `src/contour/display/*` on `yaml-cpp/emitter.h` is pre-existing on that
  tree; compile single GUI TUs via `ninja -t commands <obj>` + `cmd /c` as the memory describes.
- Linux-only shell tests (bash/zsh/fish in a PTY) run in CI; locally they `SKIP` when the shell is absent.

## Phase gate (end of every phase)

1. Full build of the touched targets with zero warnings; `ctest --test-dir out/build/clangcl-debug` green
   (record the pass count in the phase's final commit message body).
2. Run `/simplify` over the phase's diff (`git diff <phase-start>..HEAD`); commit its fixes.
3. Code review at **xhigh** effort (`/code-review xhigh` on the phase's commits, or a review subagent
   dispatched with `effort: "xhigh"`); fix every confirmed finding; commit.
4. Progress recorded (the coordinating session does this, never a subagent).

## Interface Contract

The names and signatures below are shared across phases. A phase that **produces** an item must
implement exactly this shape; a phase that **consumes** it may rely on it. If an implementer finds a
signature impossible as written, they stop and report rather than diverge silently.

### C1 — Block identity (produced by phase 1)

`src/vtbackend/shell/CommandBlock.hpp` (namespace `vtbackend`):

```cpp
namespace detail { struct CommandBlockIdTag {}; }
/// The terminal's own monotonic handle for one shell command block; zero means "no block".
using CommandBlockId = boxed::boxed<uint32_t, detail::CommandBlockIdTag>;

enum class CommandBlockState : uint8_t { Prompting = 0, Running, Finished };
enum class CommandLineSource : uint8_t { None = 0, Reported, Recovered };
enum class CommandBlockEnd : uint8_t { Reported = 0, Implicit };
enum class AdoptMode : uint8_t { Snapshot = 0, Live };
/// Which block an action addresses.
enum class CommandBlockTarget : uint8_t { Pointer = 0, Cursor, Last };

/// Longest command line a record keeps; longer ones are truncated at a UTF-8 boundary and end in "…".
constexpr inline size_t MaxRecordedCommandLineBytes = 4096;

struct WorkingDirectorySnapshot
{
    std::string path;
    ContextLocality locality = ContextLocality::Unknown;
};

struct CommandBlockRecord
{
    CommandBlockId id {};
    CommandBlockState state = CommandBlockState::Prompting;
    CommandBlockEnd end = CommandBlockEnd::Reported;
    std::string commandLine;                       // RAW; sanitise per use (C6)
    CommandLineSource commandLineSource = CommandLineSource::None;
    std::optional<int> exitCode;
    ContextOutcome outcome {};
    std::chrono::system_clock::time_point promptStartedAt {};
    std::optional<std::chrono::system_clock::time_point> commandStartedAt;
    std::optional<std::chrono::steady_clock::duration> duration;
    WorkingDirectorySnapshot workingDirectory;
    int64_t headStableId {};
    uint64_t headIdGeneration {};
    bool operator==(CommandBlockRecord const&) const = default;
};

/// What a frontend needs about a block that just finished (value copy; safe across threads).
struct CommandBlockSummary
{
    CommandBlockId id {};
    std::string commandLine;                       // RAW
    std::optional<int> exitCode;
    ContextOutcome outcome {};
    std::chrono::steady_clock::duration duration {};
    WorkingDirectorySnapshot workingDirectory;
};

struct CommandBlockStoreLimits
{
    size_t maxRecords = 1000;
    bool operator==(CommandBlockStoreLimits const&) const = default;
};

/// Where a command started, as the store needs it at OSC 133;C.
struct CommandStart
{
    std::optional<std::string> commandLine;
    CommandLineSource source = CommandLineSource::None;
    WorkingDirectorySnapshot workingDirectory;
    int64_t headStableId {};       // used only when C mints a record (no preceding A)
    uint64_t headIdGeneration {};
};

class CommandBlockStore
{
  public:
    CommandBlockStore(CommandBlockStoreLimits limits,
                      core::platform::IClock const& steadyClock,
                      core::platform::WallClockRef wallClock);

    CommandBlockId promptStarted(int64_t headStableId, uint64_t headIdGeneration);   // OSC 133;A
    void commandStarted(CommandStart start);                                         // OSC 133;C
    [[nodiscard]] std::optional<CommandBlockSummary> commandFinished(int exitCode,
                                                                     ContextOutcome outcome = {}); // ;D
    void enrichOutcome(ContextOutcome outcome);   // OSC 3008 end while OSC 133 owns the marks
    void updateHeadPosition(CommandBlockId id, int64_t headStableId, uint64_t headIdGeneration);
    [[nodiscard]] std::optional<CommandBlockSummary> adopt(CommandBlockRecord record, AdoptMode mode);
    void clear() noexcept;

    [[nodiscard]] CommandBlockRecord const* find(CommandBlockId id) const noexcept;
    [[nodiscard]] CommandBlockRecord const* current() const noexcept;   // newest unfinished, or nullptr
    [[nodiscard]] CommandBlockId currentId() const noexcept;            // id rows are stamped with
    [[nodiscard]] CommandBlockRecord const* lastFinished() const noexcept;
    template <typename F> void forEachRecord(F const& fn) const;        // oldest first
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] uint64_t revision() const noexcept;
    [[nodiscard]] CommandBlockStoreLimits const& limits() const noexcept;
};
```

`src/vtbackend/screen/TerminalClocks.hpp`:

```cpp
/// The clocks a Terminal reads; injected so tests drive time deterministically.
struct TerminalClocks
{
    core::platform::IClock const& steady;
    core::platform::WallClockRef wall;
    /// The production clocks (process-wide defaults from core::platform).
    [[nodiscard]] static TerminalClocks system() noexcept;
};
```

`Line` (`src/vtbackend/grid/Line.hpp`), tail layout `+24 bool _dirty | +25..27 uint8_t _bornAt[3] | +28..31 CommandBlockId _blockId`:

```cpp
[[nodiscard]] CommandBlockId blockId() const noexcept;
void adoptBlock(CommandBlockId id) noexcept;          // zero ignored; does NOT dirty
[[nodiscard]] uint32_t bornAt() const noexcept;        // seconds since session epoch + 1; 0 = unstamped
void stampBornAt(uint32_t secondsPlusOne) noexcept;   // only when unstamped; dirties the line once
static constexpr uint32_t MaxBornAt = (1U << 24) - 1;
```

`Screen` (`src/vtbackend/screen/Screen.hpp`): `void setActiveBlockId(CommandBlockId id) noexcept;`
(mirrors `setActiveContextId`, re-stamps and dirties the current line) and
`void setLineBirthStamp(uint32_t secondsPlusOne) noexcept;` (stamp used by `updateCursorIterator()`).

`LineFlag::UserMark` = bit 9, member of `HeadOnlyLineFlags`.

`Terminal` (`src/vtbackend/screen/Terminal.hpp`):

```cpp
Terminal(Events& eventListener, core::Environment const& env, std::unique_ptr<vtpty::Pty> pty,
         Settings factorySettings, TerminalClocks clocks,
         std::chrono::steady_clock::time_point now);
[[nodiscard]] CommandBlockStore& commandBlocks() noexcept;
[[nodiscard]] CommandBlockStore const& commandBlocks() const noexcept;
[[nodiscard]] CommandBlockRecord const* commandBlockAt(LineOffset gridLine) const noexcept; // via Line::blockId()
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> lineBirthTime(Line const& line) const noexcept;
// Terminal::Events:
virtual void commandBlockFinished(CommandBlockSummary const& /*summary*/) {}
```

`Settings` (`src/vtbackend/screen/Settings.hpp`) gains `CommandBlockStoreLimits commandBlockLimits {};`.

### C2 — Gutter (produced by phase 4)

`src/vtbackend/screen/Gutter.hpp`:

```cpp
enum class LineNumberMode : uint8_t { Off = 0, Absolute, Relative, Hybrid };
enum class GutterSegmentKind : uint8_t { Timestamp = 0, LineNumber, BlockColumn, Count };

struct GutterSettings
{
    // Engine defaults are all OFF (a default Settings reserves no gutter, as before); the user-facing
    // defaults (exitStatus/userMarks on) live in contour::config::GutterConfig.
    bool foldMarkers = false;            // replaces Settings::foldMarkers (migrated in phase 4)
    bool exitStatus = false;
    bool userMarks = false;
    LineNumberMode lineNumbers = LineNumberMode::Off;
    uint8_t lineNumberWidth = 6;         // 3..10
    bool timestamps = false;
    std::string timestampFormat = "%H:%M:%S";
    bool operator==(GutterSettings const&) const = default;
};

struct GutterSegment { GutterSegmentKind kind; int firstColumn; int columns; };   // firstColumn: 0 = gutter's left edge
struct GutterLayout
{
    std::vector<GutterSegment> segments;
    int totalColumns = 0;
    [[nodiscard]] std::optional<GutterSegment> segment(GutterSegmentKind kind) const noexcept;
};

[[nodiscard]] GutterLayout gutterLayoutFor(GutterSettings const& settings);
```

`Settings` gains `GutterSettings gutter {};`. `Grid` gains `[[nodiscard]] uint64_t evictedRowCount() const noexcept;`.
`RenderGutterCell` gains `ColumnOffset column { -1 };` (negative, relative to grid column 0).
`RenderBuffer` gains:

```cpp
struct RenderAnnotation { CellLocation position; std::u32string text; RenderAttributes attributes; };
std::vector<RenderAnnotation> annotations {};
```

`ColorPalette` gains `std::optional<RGBColor> blockStatusSuccess, blockStatusFailure, blockStatusRunning, gutterText;`
with resolvers `[[nodiscard]] RGBColor blockStatusColor(CommandBlockOutcome) const noexcept;` and
`[[nodiscard]] RGBColor gutterTextColor() const noexcept;`, where
`enum class CommandBlockOutcome : uint8_t { Success = 0, Failure, Running };` lives in `core/CommandBlockOutcome.hpp`
(re-exported by `shell/CommandBlock.hpp`) with `[[nodiscard]] CommandBlockOutcome outcomeOf(CommandBlockRecord const&) noexcept;`
in `CommandBlock.hpp` (phase 1 produces both).

contour side: `[[nodiscard]] geometry::GutterWidth gutterWidthFor(vtbackend::GutterLayout const& layout, vtbackend::ImageSize cellSize)`
in `src/contour/session/FontControl.hpp` replaces the `FoldingConfig` overload.

### C3 — Sticky header (produced by phase 7)

`src/vtbackend/shell/StickyHeader.hpp`:

```cpp
enum class StickyHeaderMode : uint8_t { Never = 0, Scrolled, Always };
struct StickyHeaderSettings
{
    StickyHeaderMode mode = StickyHeaderMode::Scrolled;
    bool showEvicted = true;
    bool operator==(StickyHeaderSettings const&) const = default;
};
enum class StickyHeaderKind : uint8_t { Command = 0, Evicted };
```

`RenderBuffer` gains `std::optional<RenderStickyHeader> stickyHeader;` with
`struct RenderStickyHeader { StickyHeaderKind kind; CommandBlockId block; std::vector<RenderCell> cells; std::u32string chip; RenderAttributes chipAttributes; RGBColor background; RGBColor separator; };`.
`Settings` gains `StickyHeaderSettings stickyHeader {};`. `Terminal` gains
`[[nodiscard]] std::optional<CommandBlockId> stickyHeaderBlock() const noexcept;` (what row 0 shows right now, for hit-testing).
`ColorPalette` gains `std::optional<RGBColor> stickyHeaderBackground, stickyHeaderSeparator;` with resolvers.

### C4 — Scrollbar marks (produced by phase 8)

`src/vtbackend/shell/ScrollbarMarks.hpp`:

```cpp
enum class ScrollbarMarkKind : uint8_t { Command = 0, UserMark, Running, Failure };   // ascending priority
enum class ScrollbarMarkSource : uint8_t { Failures = 1 << 0, Commands = 1 << 1, UserMarks = 1 << 2 };
using ScrollbarMarkSources = core::Flags<ScrollbarMarkSource>;
struct ScrollbarMarkInput { int64_t visibleRow; ScrollbarMarkKind kind; int64_t targetStableId; };
struct ScrollbarTick { int pixel; ScrollbarMarkKind kind; int64_t targetStableId; };
[[nodiscard]] std::vector<ScrollbarTick> scrollbarMarks(std::span<ScrollbarMarkInput const> inputs,
                                                        int64_t visibleRowCount, int trackPixels,
                                                        ScrollbarMarkSources sources);
```

### C5 — Block actions (produced by phase 5)

`contour/config/Actions.hpp` gains `SelectCommandBlock { vtbackend::CommandBlockTarget target; vtbackend::CommandBlockPart part; }`,
`OpenCommandOutput { vtbackend::CommandBlockTarget target; std::string program; OutputPlacement placement; OutputFormat format; }`,
`ClearToPrompt {}`, and `CopySelection { CopyFormat format; CopyFallback fallback; }` with
`enum class CopyFallback : uint8_t { None = 0, LastCommandOutput };`,
`enum class OutputPlacement : uint8_t { Split = 0, Tab, Detached };`, `enum class OutputFormat : uint8_t { Sgr = 0, Plain };`.
`src/vtbackend/input/ClickCounter.hpp`: `class ClickCounter` (constructor `(core::platform::IClock const&, std::chrono::milliseconds interval)`,
`[[nodiscard]] uint8_t press(CellLocation position, MouseButton button) noexcept`).
`MouseInputMapping` matches an optional click count (`std::optional<uint8_t> clickCount`).
`ExternalLauncher` gains `[[nodiscard]] virtual std::expected<void, SpawnError> runWithStdin(QString const& program, QStringList const& arguments, QByteArray input) = 0;`
(amended: the existing `SpawnError` and Qt types every other program-launching method there uses).

### C6 — Sanitiser (produced by phase 3)

`src/vtbackend/shell/CommandLineSanitizer.hpp`:

```cpp
enum class SanitizePurpose : uint8_t { Display = 0, Insert };
/// Display: C0/C1 controls and bidi overrides → U+FFFD; Insert: additionally drops ESC and every C0
/// except TAB and LF, and every C1.
[[nodiscard]] std::string sanitizeCommandLine(std::string_view text, SanitizePurpose purpose);
```

### C7 — Notifications (produced by phase 6)

`contour/config/Config.hpp`: `enum class NotifyWhen : uint8_t { Never = 0, Unfocused, Hidden, Always };`
`enum class NotifyOutcome : uint8_t { Any = 0, Failure };` `enum class NotifyAction : uint8_t { Notify = 0, Bell, NotifyBell };`
`enum class NotifyClearOn : uint8_t { Focus = 0, Next, Never };`
`struct FinishNotificationConfig { NotifyWhen when = NotifyWhen::Unfocused; std::chrono::seconds minDuration { 10 }; NotifyOutcome outcome = NotifyOutcome::Any; NotifyAction action = NotifyAction::Notify; NotifyClearOn clearOn = NotifyClearOn::Focus; };`
`enum class SessionVisibility : uint8_t { Focused = 0, VisibleUnfocused, Hidden };` (in `contour/platform/FinishNotification.hpp`)
`[[nodiscard]] std::optional<FinishNotificationRequest> finishNotificationFor(vtbackend::CommandBlockSummary const&, config::FinishNotificationConfig const&, SessionVisibility, std::string_view tabName);`
with `struct FinishNotificationRequest { std::string title; std::string body; config::NotifyAction action; };`.

### C8 — Picker (produced by phase 9)

`src/contour/command/RecentCommands.hpp` (Qt-free):

```cpp
struct RecentCommandRow
{
    std::string commandLine;                      // RAW
    std::optional<int> exitCode;
    std::string workingDirectory;
    std::chrono::system_clock::time_point finishedAt {};
    uint64_t sessionId {};
    std::string tabName;
};
struct RankedRecentCommand { RecentCommandRow newest; size_t count = 1; std::vector<size_t> matchPositions; };
[[nodiscard]] std::vector<RankedRecentCommand> rankRecentCommands(std::span<RecentCommandRow const> rows,
                                                                  uint64_t currentSessionId,
                                                                  std::string_view query);
```

`contour/config/Actions.hpp` gains `OpenRecentCommands {}`.

### C9 — Configuration (each feature phase adds its own; phase 10 adds settings-page rows)

| Struct (contour/config) | Scope | Added in |
|---|---|---|
| `CommandBlocksConfig { size_t maxRecords = 1000; std::string pager; }` (`pager` added in phase 5) | global, key `command_blocks` | 1, 5 |
| `GutterConfig { bool exitStatus; bool userMarks; vtbackend::LineNumberMode lineNumbers; int lineNumberWidth; bool timestamps; std::string timestampFormat; }` | global, key `gutter` | 4 |
| `ScrollBarConfig::marks` (`vtbackend::ScrollbarMarkSources`, YAML list) | profile, key `scrollbar.marks` | 8 |
| `StickyHeaderConfig { vtbackend::StickyHeaderMode mode; bool showEvicted; }` | profile, key `sticky_header` | 7 |
| `FinishNotificationConfig` (C7) | profile, key `notify_on_command_finish` | 6 |
| colour scheme slots `block_status.{success,failure,running}`, `gutter_text` | scheme | 4 |
| colour scheme slots `sticky_header.{background,separator}` | scheme | 7 |

Each struct gets a `ConfigDocumentation.hpp` entry, YAML load/store, `contour generate config` output and
a mapping into `vtbackend::Settings` where the terminal consumes it — in the phase that adds it.

### Contract amendments (settled during phase planning — binding)

The phase files implement these; where a block above disagrees, this list wins.

- **Phase 1:** `CommandBlockId` lives in `core/CommandBlockId.hpp` and `CommandBlockOutcome` in
  `core/CommandBlockOutcome.hpp` (both re-exported by `shell/CommandBlock.hpp`; `grid/` and `core/`
  may not include `shell/`). New free functions: `truncateUtf8(std::string_view, size_t maxBytes)`
  (result incl. "…" ≤ maxBytes), `truncatedCommandLine(std::string_view)`, `summaryOf(CommandBlockRecord const&)`.
  `Terminal::Events::localIdentity() const noexcept -> LocalIdentity`. `Terminal::beginCommandBlock()`,
  `beginCommand(std::optional<std::string>, CommandLineSource)`, `endCommand(int, ContextOutcome)`,
  `enrichCommandOutcome(ContextOutcome)`, `workingDirectorySnapshot() const`. `Terminal::commandBlocks()`
  lazily re-records every head after a stable-id generation change, so **consumers must read records
  through `commandBlocks()`**, never the member. `currentId()` stays the newest id after `;D`.
  `setActiveBlockId` re-stamps the cursor's whole logical line. Members `_clocks`, `_sessionEpoch`;
  config member `Config::commandBlocks`, `config::MaxCommandBlockRecords = 1'000'000`.
  `SemanticBlockTracker` API: `enable(CommandBlockId)`, `disable()`, `finishedBlocks(store, n)`,
  `inProgressBlock(store)`. `MockTerm` carries `ManualClock`/`ManualWallClock`, a local identity and a
  finished-blocks recorder.
- **Phase 2:** `Terminal::adoptCommandBlock(CommandBlockRecord, AdoptMode)`, `sessionEpoch()`,
  `adoptSessionEpoch(time_point)`; `proto::WireCommandBlock` (no head ids on the wire),
  `Delta::retiredCommandBlocks`, `SessionState::sessionEpoch`, `vthost::UnknownHeadGeneration`. The mirror
  adopts as `Snapshot` on its first replay after attaching, for a record it never held that is older than
  the newest it holds, and when re-adopting the survivors of a retirement; otherwise as `Live`, which
  announces a record's first reported finish exactly once (phase 2 D4).
- **Phase 3:** `enum class PromptKind : uint8_t { Initial = 0, Continuation, Right }` + `promptKindOf()`
  (an `A` with `k=s|c|r` starts no block); native-shell table in `contour::cli`. C6 pinned: DEL counts as
  C0; `Insert` replaces bidi controls with U+FFFD; invalid UTF-8 → one U+FFFD per broken sequence; no
  truncation.
- **Phase 4:** `vtbackend::GutterSettings` defaults every switch **off** (config defaults on).
  `session::GutterHit`, `gutterHitAt` (was `gutterLineAt`), `sendGutterHoverEvent/PressEvent(std::optional<GutterHit>…)`.
  `TerminalClocks::localTime` (`LocalTimeConverter`) + `Terminal::localTimeOf()`; timestamp helpers
  `measureTimestampFormat`, `timestampFormatErrorText`, `formatGutterTimestamp`, `TimestampRun`;
  **`vtbackend::formatCommandDuration(steady_clock::duration)`** (the one duration formatter: `850ms`,
  `12s`, `3m 05s`, `1h 02m`), used by phases 4, 6, 7.
- **Phase 5:** block actions carry `vtbackend::CommandBlockId block {}`; new `CopyCommandBlock`,
  `CopyCommandLine`; `InputBinding::clickCount`; `ClickCounter::reset()`, `DefaultInterval`, `MaxClickCount`;
  `Grid/Screen::dropRowsAbove() -> LineCount`; `TerminalSessionManager::openCommandPane`; `CommandOutputFiles`.
  Split/tab output placements are refused in daemon-attached sessions.
- **Phase 6:** C7 types in `contour/config/FinishNotificationConfig.hpp`; `finishNotificationFor(...,
  std::string_view tabName, std::string_view homeDirectory)`; `vtbackend::abbreviateHomePath(path, home)`
  (moved from `contour::window`, which no longer has it); tray messages (non-Linux) and bells route through
  the window; Linux notifications go through the session's `platform::Notifier`.
- **Phase 7:** `Viewport::scrollLineToTop(LineOffset)` and `Terminal::scrollToCommandBlockHead(CommandBlockId)
  -> std::expected<void, CommandBlockJumpError>` (the one reveal path; phase 8 builds on it);
  `RenderBufferBuilder::renderDetachedLine`; `ColorPalette::stickyHeaderBackgroundColor()`/`stickyHeaderSeparatorColor()`.
- **Phase 8:** `UserMarkIndex`, `Terminal::userMarks()`, `noteUserMarkChanged()`, `revealStableLineAtTop(int64_t,
  uint64_t) -> RevealOutcome` (on top of phase 7's `Viewport::scrollLineToTop`); `scrollbar.marks` is read by
  `TerminalSession` only (not mapped into `vtbackend::Settings`). `ScrollBarConfig::position` defaults to
  `ScrollBarPosition::Right` (Task 8.6a; spec §6.1, §11.1); phase 10 Tasks 10.7 and 10.11 rely on it.
- **Phase 9:** `TerminalSession::promptReadiness()`, `insertRecentCommand(std::string_view, RecentCommandAccept)
  -> std::expected<void, RecentCommandError>`; `RecentCommandsModel`; `utf16Indices` moved into `FuzzyFilter.hpp`.
  `RecentCommandRow` (C8) gains a last member `vtbackend::ContextLocality locality = vtbackend::ContextLocality::Unknown;`,
  filled in Task 9.2; Task 9.6's `DirectoryRole` abbreviates the home directory to `~` only when the locality is
  not `Foreign` (as phase 6 does). The action palette entry is titled "Open Recent Commands", derived from the
  action name like every other catalog row (phase 9 Decision 6); spec §9, which said "Recent commands…", is
  amended to match.
- **Phase 10:** `settings.yml` stores dotted global keys as nested YAML; the loader applies the
  `folding`, `gutter` and `command_blocks` sections (partial maps keep their siblings); timestamp
  validation reuses phase 4's `measureTimestampFormat`.

## Review Focus

Inputs and conditions the spec implies but no feature task would naturally exercise; each line's test
lives in the owning phase.

1. **Window resize (column change) while a long command is running** — the running block keeps its id,
   timestamps and recorded head position after reflow; the sticky header and scrollbar ticks still point
   at the right prompt. Tests: phase 1 (ids/timestamps survive reflow, head positions rescanned), phase 7
   (header correct after a column resize mid-output), phase 8 (ticks correct after resize).
2. **Full-screen programs on the alternate screen (vim, less, htop) while blocks exist** — no gutter
   content, no sticky header, no scrollbar ticks on the alternate screen; the picker refuses to insert;
   leaving the alternate screen restores everything. Tests: phases 4, 7, 8, 9.
3. **Hostile or pathological command lines** — 64 KiB `cmdline_url`, embedded `\e[201~`, newlines,
   bidi overrides, invalid UTF-8. Stored truncated to `MaxRecordedCommandLineBytes` at a UTF-8 boundary,
   displayed sanitised, inserted without escape sequences. Tests: phase 1 (truncation), phase 3 (sanitiser),
   phase 9 (insertion), phase 6 (notification text).
4. **A flood of OSC 133 cycles** (a program printing a recorded session, `cat` of a log that contains
   OSC 133) — memory stays bounded by `maxRecords`, each event is O(1) amortised, the gutter and
   scrollbar stay responsive. Tests: phase 1 (100k cycles keep `size() <= maxRecords`), phase 8
   (bucketing cost bounded by track pixels).
5. **No shell integration at all** (plain `cmd.exe`, a shell without the script) — every feature degrades
   to today's behaviour: no records, gutter column empty, picker shows an explanatory empty state, no
   notifications, no sticky header, the scrollbar carries no ticks (the bar itself is shown by default
   since Task 8.6a, with or without integration). Tests: phases 4, 6, 7, 8, 9 each carry a
   "no OSC 133" case.

## Spec Coverage Map

| Spec section | Phase / task file |
|---|---|
| §4 Foundation | phase 1 |
| §5 Gutter (layout, block column, line numbers, timestamps, annotations, vi `z` keys) | phase 4 (timestamp stamping: phase 1) |
| §6.1 Scrollbar marks | phase 8 |
| §6.2–6.3 Sticky header, evicted placeholder | phase 7 |
| §7 Block actions, click counts, launcher, paths | phase 5 |
| §8 Finish notifications | phase 6 |
| §9 Picker | phase 9 |
| §10 Shell scripts, sanitiser | phase 3 |
| §11 Config keys | phases 1, 4–9 (structs), phase 10 (settings page, dotted global keys) |
| §12 Daemon | phase 2 |
| §13 Performance check (Callgrind/parse throughput) | phase 10 |
| §14 Tests | every phase |
| §15 Docs, release notes | phase 10 |
| §16.1 Verification items | phase 3 (ConPTY, fish, Nushell), phase 5 (`Ctrl+Shift+G`), phase 9 (`Ctrl+Alt+R`), phase 4 (eviction counter sites), phase 10 (nested YAML leaves) |
