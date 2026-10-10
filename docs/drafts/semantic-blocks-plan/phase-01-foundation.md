# Phase 1 — Foundation: block identity and the record store

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every primary-screen row carries the `CommandBlockId` of the shell command that produced it and the second the cursor first reached it; a bounded, clock-injected `CommandBlockStore` owned by `Terminal` keeps one record per block (command line, exit status, times, working directory, head position), fed by OSC 133 and OSC 3008 through `MarkArbiter`; the dead `ShellIntegration` interface is gone, DEC mode 2034 reads the store, and vi `mm` sets a user mark that is no longer a prompt mark.

**Architecture:** `CommandBlockId` lives in `core/` so `grid/Line.hpp` can carry it in its padding; the store and its vocabulary live in `shell/CommandBlock.hpp` and are pure (manual clocks in tests). `Screen` stamps rows in its single cursor-line funnel (`updateCursorIterator`), exactly like the OSC 3008 `ContextId`; reflow carries both values on every chunk through `LogicalLineAttributes`. `Terminal` owns the store, samples the wall clock once per PTY batch, re-records block heads lazily after a stable-id generation bump, and raises `Events::commandBlockFinished` for reported ends only.

**Tech Stack:** C++23, CMake presets (`clangcl-debug`), Catch2, `core::platform` clocks (`IClock`, `WallClockRef`, `ManualClock`, `ManualWallClock`), yaml-cpp (config).

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §2, §4 (all of it) and §5.4 (the row-stamping half; the timestamp *display* is phase 4). Global constraints: [README](README.md#global-constraints). Interface contract: [README C1, C2 (`CommandBlockOutcome`, `outcomeOf`), C9 (`CommandBlocksConfig::maxRecords`)](README.md#interface-contract).

---

## Before you start

- [ ] Record the phase start: run `git rev-parse HEAD` and note the hash as `PHASE1_START` (the phase gate diffs against it).
- [ ] Every new test carries the tag `[semanticblocks]`, so `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"` runs the whole phase. Store unit tests also carry `[blockstore]`.
- [ ] Builds: `cmake --build --preset clangcl-debug --target <target>`; tests run from the worktree root as `out/build/clangcl-debug/bin/<target>.exe "<tag or name>"`.

## Contract notes (deviations and additions — the coordinator propagates these to the README)

1. **`CommandBlockId` is defined in `src/vtbackend/core/CommandBlockId.hpp`** (with a `std::formatter`) and re-exported by `shell/CommandBlock.hpp`. `grid/Line.hpp` must name the type and `grid/` may not include `shell/`; `core/ContextId.hpp` is the precedent. Every consumer that includes `CommandBlock.hpp` is unaffected.
2. `WorkingDirectorySnapshot` and `CommandBlockSummary` gain `bool operator==(…) const = default;` (`CommandBlockRecord`'s defaulted `==` requires the first).
3. Added to `CommandBlock.hpp`: `[[nodiscard]] std::string truncateUtf8(std::string_view text, size_t maxBytes);` (the one UTF-8 truncation helper of the feature — phase 6 consumes it), `[[nodiscard]] std::string truncatedCommandLine(std::string_view text);` (`truncateUtf8(text, MaxRecordedCommandLineBytes)`) and `[[nodiscard]] CommandBlockSummary summaryOf(CommandBlockRecord const& record);`.
4. `CommandBlockStore::currentId()` is the **newest** record's id whether or not it finished (rows after a `;D` still belong to the finished block until the next `;A`); `current()` is the newest record only while unfinished. A `maxRecords` of 0 is raised to 1. `forEachRecord`'s visitor may call `updateHeadPosition()` (never any other mutator).
5. `Terminal::Events` gains `[[nodiscard]] virtual LocalIdentity localIdentity() const noexcept { return {}; }` — the working-directory snapshot at `;C` needs this machine's identity for its locality, and only the session knows it. `contour::TerminalSession::localIdentity()` (already present) becomes its `override`.
6. `Terminal` gains public `void beginCommandBlock();`, `void beginCommand(std::optional<std::string> commandLine, CommandLineSource source);`, `void endCommand(int exitCode, ContextOutcome outcome);`, `void enrichCommandOutcome(ContextOutcome outcome);`, `[[nodiscard]] WorkingDirectorySnapshot workingDirectorySnapshot() const;` (the Screen → store funnel).
7. `core/LineFlags.hpp` gains `constexpr inline auto NavigationMarkFlags = LineFlags { LineFlag::Marked, LineFlag::UserMark };`.
8. `Screen::setActiveBlockId` re-stamps the cursor's whole **logical** line (head row to cursor row), not only its physical row — a prompt that starts on the last row of a wrapped output line would otherwise be handed back to the previous block by the next reflow.
9. `SemanticBlockTracker` is reshaped: `enable(CommandBlockId newestExisting)`, `disable() noexcept`, `finishedBlocks(CommandBlockStore const&, size_t)`, `inProgressBlock(CommandBlockStore const&)`; `CommandBlockInfo`, `setEnabled`, `completedBlocks`, `currentBlock`, `promptStart`, `commandOutputStart`, `commandFinished` are gone.
10. `contour::config::MaxCommandBlockRecords = 1'000'000` is the upper clamp of `command_blocks.max_records`.
11. `MockTerm` (test fixture used by later phases) gains `steadyClock`, `wallClock`, `localMachineId`, `localHostName`, `finishedCommandBlocks`.

## Decisions made here (reviewers may reject any of them)

- **Primary screen only.** Store events are fed only when the processing screen is `Terminal::primaryScreen()` (`Screen::isPrimaryPage()`); the alternate screen, DEC pages 2–15 and the status lines get flags as today but no records, no block ids and no birth stamps.
- **Mode 2034 visibility floor.** The store records whether or not 2034 is on; a 2034 reader sees only blocks with an id newer than the newest one that existed when it enabled the mode. This keeps the protocol's documented promise ("disable discards", fresh session on enable) and stops a late program from reading back earlier command lines.
- **Implicitly closed blocks** keep `duration` empty (an `ssh` closed by the remote prompt did not run "until the remote prompt").
- **Head re-recording** is lazy: `Terminal::commandBlocks()` (both overloads) first calls `syncCommandBlockHeads()`, which is a single integer compare unless `Grid::stableIdGeneration()` moved. A block's topmost surviving row is its head when it carries `Marked` or `OutputStart`; otherwise the head is recorded as evicted (`stableRangeFloor() - 1`). Imprecision accepted: a block whose prompt rows were evicted but whose `;C` row survived is re-headed at that row.
- **Reflow:** chunks created by a split take the attributes of the row they overflowed from; a continuation row rebuilt in place keeps its own (the existing `ContextId` rule). A widening rebuild gives every chunk its head's values.

---

### Task 1.1: Block identity vocabulary

**Files:**
- Create: `src/vtbackend/core/CommandBlockId.hpp`
- Create: `src/vtbackend/core/CommandBlockOutcome.hpp` (in `core/` because phase 4's `ColorPalette`, also in `core/`, resolves colours by it, and `core/` may include only `core/`)
- Create: `src/vtbackend/shell/CommandBlock.hpp`
- Create: `src/vtbackend/shell/CommandBlock.cpp`
- Test: create `src/vtbackend/shell/CommandBlock_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (headers list ~L13-87, sources list ~L89-135, `vtbackend_test` list ~L177-237)

**Interfaces:**
- Consumes: `ContextOutcome`, `ContextExit`, `ContextSignal`, `ContextLocality` (`core/TerminalContext.hpp`); `boxed::boxed`.
- Produces (C1/C2): `CommandBlockId`, `CommandBlockState`, `CommandLineSource`, `CommandBlockEnd`, `AdoptMode`, `CommandBlockTarget`, `CommandBlockOutcome`, `MaxRecordedCommandLineBytes`, `WorkingDirectorySnapshot`, `CommandBlockRecord`, `CommandBlockSummary`, `CommandBlockStoreLimits`, `CommandStart`, `[[nodiscard]] CommandBlockOutcome outcomeOf(CommandBlockRecord const&) noexcept`, plus `[[nodiscard]] std::string truncateUtf8(std::string_view text, size_t maxBytes)` (consumed by phase 6), `[[nodiscard]] std::string truncatedCommandLine(std::string_view)` and `[[nodiscard]] CommandBlockSummary summaryOf(CommandBlockRecord const&)`.

- [ ] **Step 1: Write the failing test** — create `src/vtbackend/shell/CommandBlock_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

// {{{ the vocabulary

TEST_CASE("CommandBlock.truncateUtf8.keepsWhatFits", "[semanticblocks][blockstore]")
{
    CHECK(truncateUtf8("hello", 5) == "hello");
    CHECK(truncateUtf8("hello", 64) == "hello");
    CHECK(truncateUtf8("", 0).empty());
    // Fitting wins over the ellipsis budget: nothing is cut, so nothing is appended.
    CHECK(truncateUtf8("ab", 2) == "ab");
}

TEST_CASE("CommandBlock.truncateUtf8.theEllipsisCountsAgainstTheBudget", "[semanticblocks][blockstore]")
{
    // Eight ASCII bytes into six: three kept, plus the three-byte "…".
    CHECK(truncateUtf8("abcdefgh", 6) == "abc\xE2\x80\xA6");
    CHECK(truncateUtf8("abcdefgh", 3) == "\xE2\x80\xA6");
}

TEST_CASE("CommandBlock.truncateUtf8.neverSplitsAFourByteCharacter", "[semanticblocks][blockstore]")
{
    // "a", U+1F600 (four bytes), "b": a five-byte budget leaves two bytes before the ellipsis, which
    // would end inside U+1F600 -- so the whole character goes.
    CHECK(truncateUtf8("a\xF0\x9F\x98\x80" "b", 5) == "a\xE2\x80\xA6");
}

TEST_CASE("CommandBlock.truncateUtf8.aBudgetBelowTheEllipsisYieldsNothing", "[semanticblocks][blockstore]")
{
    CHECK(truncateUtf8("abcdef", 2).empty());
    CHECK(truncateUtf8("abcdef", 0).empty());
}

TEST_CASE("CommandBlock.truncatedCommandLine.keepsWhatFits", "[semanticblocks][blockstore]")
{
    CHECK(truncatedCommandLine("ls -la") == "ls -la");
    auto const exactly = std::string(MaxRecordedCommandLineBytes, 'x');
    CHECK(truncatedCommandLine(exactly) == exactly);
}

TEST_CASE("CommandBlock.truncatedCommandLine.cutsAtACodepointAndEndsInAnEllipsis", "[semanticblocks][blockstore]")
{
    auto const ellipsis = "\xE2\x80\xA6"sv;

    SECTION("ASCII is cut at the byte budget")
    {
        auto const cut = truncatedCommandLine(std::string(MaxRecordedCommandLineBytes + 1, 'x'));
        CHECK(cut.size() == MaxRecordedCommandLineBytes);
        CHECK(cut.ends_with(ellipsis));
        CHECK(cut.substr(0, cut.size() - ellipsis.size()) == std::string(MaxRecordedCommandLineBytes - 3, 'x'));
    }

    SECTION("a three-byte character is never split")
    {
        auto text = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, 2000))
            text += "\xE2\x82\xAC"; // €
        auto const cut = truncatedCommandLine(text);
        REQUIRE(cut.ends_with(ellipsis));
        auto const kept = std::string_view { cut }.substr(0, cut.size() - ellipsis.size());
        CHECK(cut.size() <= MaxRecordedCommandLineBytes);
        CHECK(kept.size() % 3 == 0);
        CHECK(kept == std::string_view { text }.substr(0, kept.size()));
    }

    SECTION("a four-byte character is never split")
    {
        auto text = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, 1500))
            text += "\xF0\x9F\x98\x80"; // U+1F600
        auto const cut = truncatedCommandLine(text);
        REQUIRE(cut.ends_with(ellipsis));
        auto const kept = std::string_view { cut }.substr(0, cut.size() - ellipsis.size());
        CHECK(kept.size() % 4 == 0);
        CHECK(kept == std::string_view { text }.substr(0, kept.size()));
    }

    SECTION("bytes that are not UTF-8 still fit the budget")
    {
        auto const cut = truncatedCommandLine(std::string(MaxRecordedCommandLineBytes * 2, '\x80'));
        CHECK(cut.size() <= MaxRecordedCommandLineBytes);
        CHECK(cut.ends_with(ellipsis));
    }
}

TEST_CASE("CommandBlock.outcomeOf", "[semanticblocks][blockstore]")
{
    struct Row
    {
        std::string_view name;
        CommandBlockState state;
        std::optional<int> exitCode;
        ContextOutcome outcome;
        CommandBlockOutcome expected;
    };

    auto const rows = std::array {
        Row { "running", CommandBlockState::Running, std::nullopt, {}, CommandBlockOutcome::Running },
        Row { "prompting", CommandBlockState::Prompting, std::nullopt, {}, CommandBlockOutcome::Success },
        Row { "exit 0", CommandBlockState::Finished, 0, {}, CommandBlockOutcome::Success },
        Row { "exit 2", CommandBlockState::Finished, 2, {}, CommandBlockOutcome::Failure },
        Row { "implicit, no code", CommandBlockState::Finished, std::nullopt, {}, CommandBlockOutcome::Success },
        Row { "exit 0 but killed",
              CommandBlockState::Finished,
              0,
              ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv },
              CommandBlockOutcome::Failure },
        Row { "interrupted",
              CommandBlockState::Finished,
              std::nullopt,
              ContextOutcome { .exit = ContextExit::Interrupt },
              CommandBlockOutcome::Failure },
        Row { "3008 success",
              CommandBlockState::Finished,
              0,
              ContextOutcome { .exit = ContextExit::Success },
              CommandBlockOutcome::Success },
    };

    for (auto const& row: rows)
    {
        INFO(row.name);
        auto record = CommandBlockRecord {};
        record.state = row.state;
        record.exitCode = row.exitCode;
        record.outcome = row.outcome;
        CHECK(outcomeOf(record) == row.expected);
    }
}

TEST_CASE("CommandBlock.summaryOf", "[semanticblocks][blockstore]")
{
    auto record = CommandBlockRecord {};
    record.id = CommandBlockId { 3 };
    record.state = CommandBlockState::Finished;
    record.commandLine = "make";
    record.exitCode = 2;
    record.duration = 4s;
    record.workingDirectory = WorkingDirectorySnapshot { .path = "/src", .locality = ContextLocality::Local };

    CHECK(summaryOf(record)
          == CommandBlockSummary { .id = CommandBlockId { 3 },
                                   .commandLine = "make",
                                   .exitCode = 2,
                                   .outcome = {},
                                   .duration = 4s,
                                   .workingDirectory = record.workingDirectory });

    // A block closed implicitly has no duration; the summary says zero rather than inventing one.
    record.duration.reset();
    CHECK(summaryOf(record).duration == std::chrono::steady_clock::duration {});
}

// }}}
```

and register it in `src/vtbackend/CMakeLists.txt` — in the `add_executable(vtbackend_test …)` list, before `shell/CommandBlocks_test.cpp`:

```cmake
        shell/CommandBlock_test.cpp
        shell/CommandBlocks_test.cpp
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `fatal error C1083: Cannot open include file: 'vtbackend/shell/CommandBlock.hpp'`.

- [ ] **Step 3: Create `src/vtbackend/core/CommandBlockId.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>

#include <boxed-cpp/boxed.hpp>

namespace vtbackend
{

namespace detail
{
    struct CommandBlockIdTag
    {
    };
} // namespace detail

/// The terminal's own monotonic handle for one shell command block; zero means "no block".
///
/// Every row of the primary screen carries one (@see Line::blockId), in the padding after Line::_dirty,
/// so attributing a row to the command that produced it costs no memory per line. The record the id
/// names lives in CommandBlockStore, which outlives the rows: a row whose id no longer resolves simply
/// belongs to no block.
///
/// Its own header in core/, apart from shell/CommandBlock.hpp, for the reason core/ContextId.hpp is one:
/// grid/Line.hpp -- the hottest header in the tree, and below shell/ in the layering -- has to name the
/// type without acquiring the store, the clocks and <deque>.
using CommandBlockId = boxed::boxed<uint32_t, detail::CommandBlockIdTag>;

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::CommandBlockId>: formatter<uint32_t> // NOLINT(readability-identifier-naming)
{
    auto format(vtbackend::CommandBlockId id, auto& ctx) const
    {
        return formatter<uint32_t>::format(id.value, ctx);
    }
};
```

- [ ] **Step 3b: Create `src/vtbackend/core/CommandBlockOutcome.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// How a command block reads at a glance -- what the gutter and the scrollbar colour it by.
///
/// In core/ rather than shell/CommandBlock.hpp because ColorPalette (core/) resolves a colour per
/// outcome, and core/ may include nothing but core/. shell/CommandBlock.hpp includes this header, so
/// every consumer of the block vocabulary sees it under the same name.
enum class CommandBlockOutcome : uint8_t
{
    Success = 0, ///< Exit code zero, no command run yet, or nothing known.
    Failure,     ///< A non-zero exit code, a signal, a crash or an interrupt.
    Running,     ///< The command is still running.
};

} // namespace vtbackend
```

- [ ] **Step 4: Create `src/vtbackend/shell/CommandBlock.hpp`** (vocabulary only; the store follows in Task 1.2)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/core/TerminalContext.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

/// Where a command block is in its cycle.
enum class CommandBlockState : uint8_t
{
    Prompting = 0, ///< The prompt is up (OSC 133;A); nothing has run yet.
    Running,       ///< The command started (OSC 133;C) and has not reported its end.
    Finished,      ///< The command ended: reported (;D) or implied (a newer prompt superseded it).
};

/// Where a record's command line came from.
enum class CommandLineSource : uint8_t
{
    None = 0,  ///< Nobody said: no cmdline_url, and no ;B to read the typed text from.
    Reported,  ///< The shell sent it (OSC 133;C cmdline_url=, or an OSC 3008 cmdline=).
    Recovered, ///< Read off the grid, between the ;B column and the output start.
};

/// How a Finished block ended.
enum class CommandBlockEnd : uint8_t
{
    Reported = 0, ///< OSC 133;D, or an OSC 3008 end= while OSC 3008 owns the marks.
    Implicit,     ///< A newer prompt superseded it: a nested shell's prompt, or a missing ;D.
};

/// How a record replicated from a daemon host enters the store. @see CommandBlockStore::adopt.
enum class AdoptMode : uint8_t
{
    Snapshot = 0, ///< An attach snapshot: history, so no finish is announced.
    Live,         ///< A live delta: a first transition to a reported Finished is announced.
};

/// Which block an action addresses.
enum class CommandBlockTarget : uint8_t
{
    Pointer = 0, ///< The block under the mouse pointer.
    Cursor,      ///< The block the cursor (the vi cursor in vi mode) is in.
    Last,        ///< The most recently finished block.
};

/// Longest command line a record keeps; longer ones are truncated at a UTF-8 boundary and end in "…".
constexpr inline size_t MaxRecordedCommandLineBytes = 4096;

/// The working directory a command ran in, as resolved at its OSC 133;C.
struct WorkingDirectorySnapshot
{
    std::string path;                                   ///< Empty when nothing reported one.
    ContextLocality locality = ContextLocality::Unknown; ///< Whether @ref path names this machine.

    bool operator==(WorkingDirectorySnapshot const&) const = default;
};

/// Everything the terminal knows about one block that the grid cannot hold.
struct CommandBlockRecord
{
    CommandBlockId id {};                               ///< Never zero for a stored record.
    CommandBlockState state = CommandBlockState::Prompting; ///< Where the block is in its cycle.
    CommandBlockEnd end = CommandBlockEnd::Reported;    ///< Meaningful once Finished.
    std::string commandLine;                            ///< RAW, as reported or recovered; sanitise per use (C6).
    CommandLineSource commandLineSource = CommandLineSource::None; ///< Where @ref commandLine came from.
    std::optional<int> exitCode;                        ///< From ;D, or ContextOutcome::asShellExitCode().
    ContextOutcome outcome {};                          ///< Signal / crash / interrupt, when OSC 3008 said.
    std::chrono::system_clock::time_point promptStartedAt {};                ///< Wall clock at ;A.
    std::optional<std::chrono::system_clock::time_point> commandStartedAt;   ///< Wall clock at ;C.
    std::optional<std::chrono::steady_clock::duration> duration;            ///< ;C to a reported end.
    WorkingDirectorySnapshot workingDirectory;          ///< Snapshot at ;C.
    int64_t headStableId {};                            ///< The head row's stable id, valid in @ref headIdGeneration.
    uint64_t headIdGeneration {};                       ///< The Grid::stableIdGeneration() of @ref headStableId.

    bool operator==(CommandBlockRecord const&) const = default;
};

/// What a frontend needs about a block that just finished (value copy; safe across threads).
struct CommandBlockSummary
{
    CommandBlockId id {};                            ///< The block that finished.
    std::string commandLine;                         ///< RAW
    std::optional<int> exitCode;                     ///< As reported by ;D, when reported.
    ContextOutcome outcome {};                       ///< Signal / crash / interrupt, when OSC 3008 said.
    std::chrono::steady_clock::duration duration {}; ///< ;C to the reported end; zero when unknown.
    WorkingDirectorySnapshot workingDirectory;       ///< Snapshot at ;C.

    bool operator==(CommandBlockSummary const&) const = default;
};

/// How much a CommandBlockStore keeps.
struct CommandBlockStoreLimits
{
    size_t maxRecords = 1000; ///< The oldest records are dropped first beyond this.

    bool operator==(CommandBlockStoreLimits const&) const = default;
};

/// Where a command started, as the store needs it at OSC 133;C.
struct CommandStart
{
    std::optional<std::string> commandLine;            ///< Taken only when @ref source is not None.
    CommandLineSource source = CommandLineSource::None; ///< Who supplied @ref commandLine; None means nobody did.
    WorkingDirectorySnapshot workingDirectory;
    int64_t headStableId {};    ///< Used only when ;C mints a record (no preceding ;A).
    uint64_t headIdGeneration {}; ///< The Grid::stableIdGeneration() of @ref headStableId.
};

/// @p text cut to at most @p maxBytes bytes at a UTF-8 code point boundary, ending in "…" when anything
/// was cut. The one UTF-8 truncation helper of the feature: the record's command line, a notification's
/// command name (phase 6), any other bounded display of untrusted text.
///
/// The ellipsis (three bytes) counts against @p maxBytes, so the result never exceeds it. Text that is
/// not UTF-8 at the cut is cut there anyway -- the bytes are kept raw, and the sanitiser (phase 3) owns
/// making them presentable.
/// @param text The text as received.
/// @param maxBytes The most bytes the result may have, ellipsis included.
/// @return @p text itself when it fits; else its longest whole-code-point prefix plus "…"; an empty string
///         when @p text does not fit and @p maxBytes cannot even hold the ellipsis.
[[nodiscard]] std::string truncateUtf8(std::string_view text, size_t maxBytes);

/// @p text cut to MaxRecordedCommandLineBytes, at a UTF-8 code point boundary, ending in "…".
///
/// A command line arrives from whatever wrote to the pty, so its length is the writer's choice; this is
/// what bounds what a record costs.
/// @param text The command line as received.
/// @return truncateUtf8(@p text, MaxRecordedCommandLineBytes).
[[nodiscard]] std::string truncatedCommandLine(std::string_view text);

/// How @p record reads at a glance. A block with no exit code known reads as a success.
/// @param record The block.
/// @return Running while running; Failure for a non-zero exit code or a non-successful OSC 3008 end.
[[nodiscard]] CommandBlockOutcome outcomeOf(CommandBlockRecord const& record) noexcept;

/// The value a frontend is handed when @p record finishes.
/// @param record The block.
/// @return Its summary; a block with no duration reports zero.
[[nodiscard]] CommandBlockSummary summaryOf(CommandBlockRecord const& record);

} // namespace vtbackend
```

- [ ] **Step 5: Create `src/vtbackend/shell/CommandBlock.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>

#include <ranges>

namespace vtbackend
{

namespace
{
    /// U+2026 HORIZONTAL ELLIPSIS, which a truncated command line ends in.
    constexpr auto Ellipsis = std::string_view { "\xE2\x80\xA6" };

    /// The longest tail a UTF-8 sequence has: a four-byte sequence carries three continuation bytes.
    constexpr auto MaxContinuationBytes = size_t { 3 };

    /// Whether @p octet continues a UTF-8 sequence (10xxxxxx) rather than starting one.
    [[nodiscard]] constexpr bool isContinuationByte(char octet) noexcept
    {
        return (static_cast<unsigned>(static_cast<unsigned char>(octet)) & 0b1100'0000U) == 0b1000'0000U;
    }
} // namespace

std::string truncateUtf8(std::string_view text, size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return std::string { text };
    if (maxBytes < Ellipsis.size())
        return {};

    // Back off to the start of a code point so the ellipsis never splits one. Three steps at most: a
    // longer run of continuation bytes is not UTF-8 at all, and is cut wherever the budget leaves it.
    // text[cut] is in range: cut < maxBytes < text.size().
    auto cut = maxBytes - Ellipsis.size();
    for ([[maybe_unused]] auto const step: std::views::iota(size_t { 0 }, MaxContinuationBytes))
    {
        if (cut == 0 || !isContinuationByte(text[cut]))
            break;
        --cut;
    }

    auto result = std::string { text.substr(0, cut) };
    result += Ellipsis;
    return result;
}

std::string truncatedCommandLine(std::string_view text)
{
    return truncateUtf8(text, MaxRecordedCommandLineBytes);
}

CommandBlockOutcome outcomeOf(CommandBlockRecord const& record) noexcept
{
    if (record.state == CommandBlockState::Running)
        return CommandBlockOutcome::Running;

    auto const failedByCode = record.exitCode.has_value() && *record.exitCode != 0;
    auto const failedByOutcome = record.outcome.signal != ContextSignal::None
                                 || (record.outcome.exit != ContextExit::Unknown
                                     && record.outcome.exit != ContextExit::Success);
    return (failedByCode || failedByOutcome) ? CommandBlockOutcome::Failure : CommandBlockOutcome::Success;
}

CommandBlockSummary summaryOf(CommandBlockRecord const& record)
{
    return CommandBlockSummary {
        .id = record.id,
        .commandLine = record.commandLine,
        .exitCode = record.exitCode,
        .outcome = record.outcome,
        .duration = record.duration.value_or(std::chrono::steady_clock::duration {}),
        .workingDirectory = record.workingDirectory,
    };
}

} // namespace vtbackend
```

- [ ] **Step 6: Register the files in `src/vtbackend/CMakeLists.txt`**

In `set(vtbackend_HEADERS …)`, after `core/ContextId.hpp` add `core/CommandBlockId.hpp` and `core/CommandBlockOutcome.hpp`, and before `shell/CommandBlocks.hpp` add `shell/CommandBlock.hpp`:

```cmake
    core/ContextId.hpp
    core/CommandBlockId.hpp
    core/CommandBlockOutcome.hpp
    core/ColorPalette.hpp
    shell/CommandBlock.hpp
    shell/CommandBlocks.hpp
```

In `set(vtbackend_SOURCES …)`, before `shell/CommandBlocks.cpp`:

```cmake
    shell/CommandBlock.cpp
    shell/CommandBlocks.cpp
```

- [ ] **Step 7: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[blockstore]"`
Expected: build clean (zero warnings); `All tests passed`.

- [ ] **Step 8: Format and commit**

Run: `clang-format -i src/vtbackend/core/CommandBlockId.hpp src/vtbackend/core/CommandBlockOutcome.hpp src/vtbackend/shell/CommandBlock.hpp src/vtbackend/shell/CommandBlock.cpp src/vtbackend/shell/CommandBlock_test.cpp`

```bash
git add src/vtbackend/core/CommandBlockId.hpp src/vtbackend/core/CommandBlockOutcome.hpp src/vtbackend/shell/CommandBlock.hpp src/vtbackend/shell/CommandBlock.cpp src/vtbackend/shell/CommandBlock_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: add the command block vocabulary

CommandBlockId, the record and summary types, truncateUtf8() and the
command-line truncation built on it (both cut at a UTF-8 boundary, the
ellipsis inside the budget) and outcomeOf(). The id lives in core/ so
grid/Line.hpp can carry it without reaching into shell/.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.2: CommandBlockStore — the transition table and the cap

**Files:**
- Modify: `src/vtbackend/shell/CommandBlock.hpp` (append the class after `summaryOf`; add includes)
- Modify: `src/vtbackend/shell/CommandBlock.cpp` (append the implementation)
- Test: `src/vtbackend/shell/CommandBlock_test.cpp` (append)

**Interfaces:**
- Consumes: Task 1.1 types; `core::platform::IClock`, `core::platform::WallClockRef`, `core::platform::SteadyTimePoint` (`<core/platform/Clock.hpp>`).
- Produces (C1): `CommandBlockStore(CommandBlockStoreLimits, core::platform::IClock const&, core::platform::WallClockRef)`, `CommandBlockId promptStarted(int64_t, uint64_t)`, `void commandStarted(CommandStart)`, `[[nodiscard]] std::optional<CommandBlockSummary> commandFinished(int exitCode, ContextOutcome outcome = {})`, `void clear() noexcept`, `find`, `current`, `currentId`, `lastFinished`, `forEachRecord`, `size`, `revision`, `limits` (exact C1 signatures). `enrichOutcome`, `updateHeadPosition` and `adopt` follow in Task 1.3.

- [ ] **Step 1: Write the failing tests** — append to `src/vtbackend/shell/CommandBlock_test.cpp`:

```cpp
// {{{ the store

namespace
{

/// A store on manual clocks: time moves only when the test says so.
struct StoreFixture
{
    core::platform::ManualClock steady;
    core::platform::ManualWallClock wall { std::chrono::system_clock::time_point { std::chrono::hours { 24 } } };
    CommandBlockStore store;

    explicit StoreFixture(CommandBlockStoreLimits limits = {}): store { limits, steady, wall } {}
};

/// An OSC 133;C that carries a cmdline_url and nothing else.
[[nodiscard]] CommandStart reported(std::string commandLine)
{
    return CommandStart { .commandLine = std::move(commandLine), .source = CommandLineSource::Reported };
}

} // namespace

TEST_CASE("CommandBlockStore.aPromptMintsAPromptingRecord", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const id = f.store.promptStarted(10, 3);

    CHECK(id == CommandBlockId { 1 });
    REQUIRE(f.store.size() == 1);
    auto const* record = f.store.current();
    REQUIRE(record != nullptr);
    CHECK(record->id == id);
    CHECK(record->state == CommandBlockState::Prompting);
    CHECK(record->promptStartedAt == f.wall.now());
    CHECK(record->headStableId == 10);
    CHECK(record->headIdGeneration == 3);
    CHECK(f.store.currentId() == id);
    CHECK(f.store.lastFinished() == nullptr);
}

TEST_CASE("CommandBlockStore.aFullCycleFinishesWithExitCodeDurationAndCommandLine", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const id = f.store.promptStarted(0, 0);
    f.wall.advance(2s);
    f.steady.advance(2s);
    auto const startedAt = f.wall.now();
    f.store.commandStarted(
        CommandStart { .commandLine = "make -j8",
                       .source = CommandLineSource::Reported,
                       .workingDirectory = { .path = "/src", .locality = ContextLocality::Local } });
    f.steady.advance(3s);
    f.wall.advance(3s);
    auto const summary = f.store.commandFinished(2);

    REQUIRE(summary.has_value());
    CHECK(summary->id == id);
    CHECK(summary->commandLine == "make -j8");
    CHECK(summary->exitCode == 2);
    CHECK(summary->duration == 3s);
    CHECK(summary->workingDirectory.path == "/src");

    auto const* record = f.store.find(id);
    REQUIRE(record != nullptr);
    CHECK(record->state == CommandBlockState::Finished);
    CHECK(record->end == CommandBlockEnd::Reported);
    CHECK(record->commandStartedAt == startedAt);
    CHECK(record->duration == 3s);
    CHECK(record->workingDirectory.locality == ContextLocality::Local);
    CHECK(f.store.current() == nullptr);
    CHECK(f.store.lastFinished() == record);
    // Rows the cursor reaches after the ;D still belong to it, until the next prompt starts.
    CHECK(f.store.currentId() == id);
}

TEST_CASE("CommandBlockStore.aPromptThatRanNothingIsDiscardedByTheNext", "[semanticblocks][blockstore]")
{
    // An empty Enter, or Ctrl-C at the prompt: the shell draws a fresh prompt and never emits ;C.
    auto f = StoreFixture {};
    auto const first = f.store.promptStarted(0, 0);
    auto const second = f.store.promptStarted(1, 0);

    CHECK(f.store.size() == 1);
    CHECK(f.store.find(first) == nullptr);
    CHECK(f.store.find(second) != nullptr);
    CHECK(second != first);
}

TEST_CASE("CommandBlockStore.aNewPromptClosesARunningBlockImplicitly", "[semanticblocks][blockstore]")
{
    // `ssh host` runs and the REMOTE shell's prompt arrives: the outer command has not finished, so its
    // block is closed without an exit code -- and promptStarted() returns no summary to announce.
    auto f = StoreFixture {};
    auto const outer = f.store.promptStarted(0, 0);
    f.store.commandStarted(reported("ssh host"));
    auto const inner = f.store.promptStarted(5, 0);

    auto const* closed = f.store.find(outer);
    REQUIRE(closed != nullptr);
    CHECK(closed->state == CommandBlockState::Finished);
    CHECK(closed->end == CommandBlockEnd::Implicit);
    CHECK_FALSE(closed->exitCode.has_value());
    CHECK_FALSE(closed->duration.has_value());
    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->id == inner);
    CHECK(f.store.current()->state == CommandBlockState::Prompting);
    // The ;D the remote shell's precmd sends belongs to no command this store saw start.
    CHECK_FALSE(f.store.commandFinished(0).has_value());
}

TEST_CASE("CommandBlockStore.aCommandStartWithoutAPromptMintsItsOwnRecord", "[semanticblocks][blockstore]")
{
    // A shell integration sourced halfway through a session: its first sequence may well be a ;C.
    auto f = StoreFixture {};
    f.store.commandStarted(CommandStart { .headStableId = 42, .headIdGeneration = 7 });

    auto const* record = f.store.current();
    REQUIRE(record != nullptr);
    CHECK(record->state == CommandBlockState::Running);
    CHECK(record->headStableId == 42);
    CHECK(record->headIdGeneration == 7);
    CHECK(record->commandStartedAt == record->promptStartedAt);
    CHECK(record->commandLineSource == CommandLineSource::None);
}

TEST_CASE("CommandBlockStore.aFinishWithNothingRunningIsIgnored", "[semanticblocks][blockstore]")
{
    // tcsh emits ;D unconditionally; a nested shell's exit leaves one behind at the outer prompt.
    auto f = StoreFixture {};
    CHECK_FALSE(f.store.commandFinished(1).has_value());
    CHECK(f.store.size() == 0);

    f.store.promptStarted(0, 0);
    auto const revision = f.store.revision();
    CHECK_FALSE(f.store.commandFinished(0).has_value());
    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->state == CommandBlockState::Prompting);
    CHECK(f.store.revision() == revision);
}

TEST_CASE("CommandBlockStore.aRepeatedCommandStartFillsButNeverReplacesTheCommandLine",
          "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    f.store.promptStarted(0, 0);
    f.store.commandStarted(CommandStart {});
    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->commandLineSource == CommandLineSource::None);

    f.store.commandStarted(reported("ls"));
    CHECK(f.store.current()->commandLine == "ls");
    CHECK(f.store.current()->commandLineSource == CommandLineSource::Reported);

    f.store.commandStarted(reported("rm -rf /"));
    CHECK(f.store.current()->commandLine == "ls");
}

TEST_CASE("CommandBlockStore.theWorkingDirectoryIsTheOneInEffectAtTheCommandStart",
          "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    f.store.promptStarted(0, 0);
    f.store.commandStarted(CommandStart { .workingDirectory = { .path = "/a", .locality = ContextLocality::Local } });
    f.store.commandStarted(CommandStart { .workingDirectory = { .path = "/b", .locality = ContextLocality::Foreign } });

    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->workingDirectory
          == WorkingDirectorySnapshot { .path = "/a", .locality = ContextLocality::Local });
}

TEST_CASE("CommandBlockStore.aHugeCommandLineIsStoredTruncatedAtACodepoint", "[semanticblocks][blockstore]")
{
    // Review Focus #3, at the store: whatever wrote to the pty decides how long a cmdline_url is.
    auto f = StoreFixture {};
    auto hugeLine = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, (64 * 1024 / 3) + 1))
        hugeLine += "\xE2\x82\xAC"; // €
    f.store.commandStarted(reported(hugeLine));

    auto const* record = f.store.current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.size() <= MaxRecordedCommandLineBytes);
    REQUIRE(record->commandLine.ends_with("\xE2\x80\xA6"));
    auto const kept = std::string_view { record->commandLine }.substr(0, record->commandLine.size() - 3);
    CHECK(kept.size() % 3 == 0);
    CHECK(kept == std::string_view { hugeLine }.substr(0, kept.size()));
}

TEST_CASE("CommandBlockStore.theRevisionMovesWithEveryChangeAndOnlyThen", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto last = f.store.revision();
    auto const moved = [&] {
        auto const now = f.store.revision();
        auto const changed = now != last;
        last = now;
        return changed;
    };

    f.store.promptStarted(0, 0);
    CHECK(moved());
    f.store.commandStarted(reported("ls"));
    CHECK(moved());
    std::ignore = f.store.commandFinished(0);
    CHECK(moved());
    std::ignore = f.store.commandFinished(0); // nothing running: ignored
    CHECK_FALSE(moved());
    f.store.clear();
    CHECK(moved());
}

TEST_CASE("CommandBlockStore.clearForgetsEverythingButNeverReusesAnId", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const before = f.store.promptStarted(0, 0);
    f.store.commandStarted(reported("ls"));
    f.store.clear();

    CHECK(f.store.size() == 0);
    CHECK(f.store.current() == nullptr);
    CHECK(f.store.currentId() == CommandBlockId {});
    // A row stamped before the reset must never resolve to a block that came after it.
    CHECK(f.store.promptStarted(0, 0) > before);
}

TEST_CASE("CommandBlockStore.theCapDropsTheOldestFirst", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture { CommandBlockStoreLimits { .maxRecords = 3 } };
    auto ids = std::vector<CommandBlockId> {};
    for (auto const i: std::views::iota(0, 5))
    {
        ids.push_back(f.store.promptStarted(i, 0));
        f.store.commandStarted(CommandStart {});
        std::ignore = f.store.commandFinished(0);
    }

    CHECK(f.store.size() == 3);
    CHECK(f.store.find(ids[0]) == nullptr);
    CHECK(f.store.find(ids[1]) == nullptr);
    CHECK(f.store.find(ids[2]) != nullptr);
    CHECK(f.store.find(ids[4]) != nullptr);
}

TEST_CASE("CommandBlockStore.aCapOfZeroStillKeepsTheCurrentBlock", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture { CommandBlockStoreLimits { .maxRecords = 0 } };
    CHECK(f.store.limits().maxRecords == 1);
    auto const id = f.store.promptStarted(0, 0);
    CHECK(f.store.find(id) != nullptr);
}

TEST_CASE("CommandBlockStore.aFloodOfCyclesStaysWithinTheCap", "[semanticblocks][blockstore]")
{
    // Review Focus #4: a `cat` of a recorded session replays OSC 133 as fast as the pty delivers it.
    auto f = StoreFixture { CommandBlockStoreLimits { .maxRecords = 1000 } };
    auto largest = size_t { 0 };
    auto lastId = CommandBlockId {};
    for (auto const i: std::views::iota(0, 100'000))
    {
        lastId = f.store.promptStarted(i, 0);
        f.store.commandStarted(CommandStart {});
        std::ignore = f.store.commandFinished(0);
        largest = std::max(largest, f.store.size());
    }

    CHECK(largest <= 1000);
    CHECK(f.store.size() == 1000);
    REQUIRE(f.store.lastFinished() != nullptr);
    CHECK(f.store.lastFinished()->id == lastId);
    CHECK(f.store.find(CommandBlockId { 1 }) == nullptr); // the oldest went first
}

// }}}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2065: 'CommandBlockStore': undeclared identifier` (in `StoreFixture`).

- [ ] **Step 3: Add the class to `src/vtbackend/shell/CommandBlock.hpp`**

Add to the includes:

```cpp
#include <core/platform/Clock.hpp>

#include <deque>
```

Append after the `summaryOf` declaration, before `} // namespace vtbackend`:

```cpp
/// Every command block this terminal knows of, oldest first, bounded by CommandBlockStoreLimits.
///
/// Fed by OSC 133 -- and, while OSC 3008 owns the marks, by its command contexts -- through Terminal,
/// and read by the gutter, the scrollbar marks, the sticky header, finish notifications and the
/// recent-commands picker. Deliberately free of Screen, Terminal and the grid: given an event it
/// produces the next state, so every transition is exercised with manual clocks and nothing behind it.
/// ContextStack is the model.
///
/// Invariants: ids strictly ascend from oldest to newest; only the NEWEST record can be unfinished (the
/// next prompt finishes or discards it); the newest record is never evicted.
///
/// Every transition is TOTAL: the protocol is lenient, so a malformed order (a ;D with nothing running, a
/// ;C with no ;A) is an outcome, never an error.
class CommandBlockStore
{
  public:
    /// @param limits How many records to keep. Fixed for the store's life; a maxRecords of zero is raised
    ///        to one, because the block rows are being stamped with must always resolve.
    /// @param steadyClock Measures how long commands run. Borrowed: it must outlive the store.
    /// @param wallClock Says when prompts and commands started. Borrowed likewise.
    CommandBlockStore(CommandBlockStoreLimits limits,
                      core::platform::IClock const& steadyClock,
                      core::platform::WallClockRef wallClock);

    /// OSC 133;A: a new prompt starts.
    ///
    /// A Prompting record that never reached ;C is DISCARDED (an empty Enter, Ctrl-C at the prompt); a
    /// Running one is closed as Finished with CommandBlockEnd::Implicit, no exit code and no duration --
    /// a nested shell's prompt (ssh, a subshell) or a missing ;D. Nothing is announced for it: the `ssh`
    /// the user is still inside has not finished. Then a new Prompting record is minted.
    /// @param headStableId The stable id of the prompt's head row.
    /// @param headIdGeneration The Grid::stableIdGeneration() that id is valid in.
    /// @return The new record's id.
    CommandBlockId promptStarted(int64_t headStableId, uint64_t headIdGeneration);

    /// OSC 133;C: the current block's command starts.
    ///
    /// A Prompting record becomes Running, with its start time and working directory. With no unfinished
    /// record at all (a shell integration sourced mid-session) one is minted at @p start's head. A command
    /// line is recorded once, truncated to MaxRecordedCommandLineBytes: a repeated ;C may fill a missing
    /// one, never replace one.
    /// @param start What the terminal knew at the ;C.
    void commandStarted(CommandStart start);

    /// OSC 133;D, or an OSC 3008 `end=` while OSC 3008 owns the marks: the running command finished.
    ///
    /// Ignored when nothing is Running -- tcsh's unconditional ;D, a nested shell's exit.
    /// @param exitCode The exit code the shell reported.
    /// @param outcome How the command ended, when OSC 3008 said; a default outcome keeps one an earlier
    ///        enrichOutcome() recorded.
    /// @return What a frontend needs to announce the finish, or nullopt when nothing finished.
    [[nodiscard]] std::optional<CommandBlockSummary> commandFinished(int exitCode, ContextOutcome outcome = {});

    /// Forgets every record. Ids are NOT reused afterwards. Used by RIS / hard reset, and by a daemon
    /// mirror rebuilding its store after retirement (phase 2 Task 2.9).
    void clear() noexcept;

    /// @return The record @p id names, or nullptr when it was never minted, was discarded or evicted.
    [[nodiscard]] CommandBlockRecord const* find(CommandBlockId id) const noexcept;

    /// @return The newest record while it is unfinished (Prompting or Running), else nullptr.
    [[nodiscard]] CommandBlockRecord const* current() const noexcept;

    /// @return The id freshly reached rows are stamped with: the newest record's, or zero when empty.
    [[nodiscard]] CommandBlockId currentId() const noexcept;

    /// @return The newest Finished record, however it ended, or nullptr.
    [[nodiscard]] CommandBlockRecord const* lastFinished() const noexcept;

    /// Visits every record, oldest first.
    /// @param fn Called as fn(CommandBlockRecord const&).
    template <typename F>
    void forEachRecord(F const& fn) const
    {
        for (auto const& record: _records)
            fn(record);
    }

    /// @return How many records are kept.
    [[nodiscard]] size_t size() const noexcept { return _records.size(); }

    /// @return A counter bumped by every change; caches (gutter, scrollbar marks, sticky header) key on it.
    [[nodiscard]] uint64_t revision() const noexcept { return _revision; }

    /// @return The limits in force (maxRecords already raised to at least one).
    [[nodiscard]] CommandBlockStoreLimits const& limits() const noexcept { return _limits; }

  private:
    /// The newest record while unfinished, for mutation. @see current.
    [[nodiscard]] CommandBlockRecord* unfinished() noexcept;

    /// Appends a Prompting record headed at the given row, evicting the oldest beyond the cap.
    CommandBlockRecord& mint(int64_t headStableId, uint64_t headIdGeneration);

    /// Drops the oldest records until the cap holds.
    void evictOverflow() noexcept;

    CommandBlockStoreLimits _limits;
    core::platform::IClock const& _steadyClock;
    core::platform::WallClockRef _wallClock;
    std::deque<CommandBlockRecord> _records;

    /// When the running command started, by the steady clock. One field suffices: the one Running record
    /// is always the newest.
    std::optional<core::platform::SteadyTimePoint> _runningSince;

    uint32_t _nextId = 1; ///< Never zero; zero means "no block".
    uint64_t _revision = 0;
};
```

- [ ] **Step 4: Implement it** — in `src/vtbackend/shell/CommandBlock.cpp`, replace `#include <ranges>` with:

```cpp
#include <algorithm>
#include <functional>
#include <limits>
#include <ranges>
#include <utility>
```

add to the anonymous namespace, after `isContinuationByte`:

```cpp
    /// Where @p id sits in @p records (sorted by id), or end(). Shared by the const and mutable lookups so
    /// the search is written once.
    template <typename Records>
    [[nodiscard]] auto positionOf(Records& records, CommandBlockId id) noexcept
    {
        auto const it = std::ranges::lower_bound(records, id, std::less {}, &CommandBlockRecord::id);
        return (it != records.end() && it->id == id) ? it : records.end();
    }

    /// The newest record of @p records while it is unfinished, else nullptr.
    template <typename Records>
    [[nodiscard]] auto newestUnfinishedIn(Records& records) noexcept -> decltype(&records.back())
    {
        if (records.empty() || records.back().state == CommandBlockState::Finished)
            return nullptr;
        return &records.back();
    }

    /// The newest Finished record of @p records, else nullptr.
    template <typename Records>
    [[nodiscard]] auto newestFinishedIn(Records& records) noexcept -> decltype(&records.back())
    {
        auto const reversed = records | std::views::reverse;
        auto const it = std::ranges::find(reversed, CommandBlockState::Finished, &CommandBlockRecord::state);
        if (it == reversed.end())
            return nullptr;
        return &*it;
    }
```

and append after `summaryOf`:

```cpp
CommandBlockStore::CommandBlockStore(CommandBlockStoreLimits limits,
                                     core::platform::IClock const& steadyClock,
                                     core::platform::WallClockRef wallClock):
    _limits { .maxRecords = std::max(limits.maxRecords, size_t { 1 }) },
    _steadyClock { steadyClock },
    _wallClock { wallClock }
{
}

CommandBlockId CommandBlockStore::promptStarted(int64_t headStableId, uint64_t headIdGeneration)
{
    if (auto* const open = unfinished())
    {
        if (open->state == CommandBlockState::Prompting)
            _records.pop_back(); // unfinished() is always the newest record
        else
        {
            open->state = CommandBlockState::Finished;
            open->end = CommandBlockEnd::Implicit;
            _runningSince.reset();
        }
    }

    auto const id = mint(headStableId, headIdGeneration).id;
    ++_revision;
    return id;
}

void CommandBlockStore::commandStarted(CommandStart start)
{
    auto* record = unfinished();
    if (!record)
        record = &mint(start.headStableId, start.headIdGeneration);

    if (record->state == CommandBlockState::Prompting)
    {
        record->state = CommandBlockState::Running;
        record->commandStartedAt = _wallClock.now();
        record->workingDirectory = std::move(start.workingDirectory);
        _runningSince = _steadyClock.now();
    }

    if (record->commandLineSource == CommandLineSource::None && start.source != CommandLineSource::None
        && start.commandLine)
    {
        record->commandLine = truncatedCommandLine(*start.commandLine);
        record->commandLineSource = start.source;
    }

    ++_revision;
}

std::optional<CommandBlockSummary> CommandBlockStore::commandFinished(int exitCode, ContextOutcome outcome)
{
    auto* const record = unfinished();
    if (!record || record->state != CommandBlockState::Running)
        return std::nullopt;

    record->state = CommandBlockState::Finished;
    record->end = CommandBlockEnd::Reported;
    record->exitCode = exitCode;
    if (outcome != ContextOutcome {})
        record->outcome = outcome;
    if (_runningSince)
        record->duration = _steadyClock.now() - *_runningSince;
    _runningSince.reset();
    ++_revision;
    return summaryOf(*record);
}

void CommandBlockStore::clear() noexcept
{
    _records.clear();
    _runningSince.reset();
    ++_revision;
}

CommandBlockRecord const* CommandBlockStore::find(CommandBlockId id) const noexcept
{
    if (!id)
        return nullptr;
    auto const it = positionOf(_records, id);
    return it == _records.end() ? nullptr : &*it;
}

CommandBlockRecord const* CommandBlockStore::current() const noexcept
{
    return newestUnfinishedIn(_records);
}

CommandBlockId CommandBlockStore::currentId() const noexcept
{
    return _records.empty() ? CommandBlockId {} : _records.back().id;
}

CommandBlockRecord const* CommandBlockStore::lastFinished() const noexcept
{
    return newestFinishedIn(_records);
}

CommandBlockRecord* CommandBlockStore::unfinished() noexcept
{
    return newestUnfinishedIn(_records);
}

CommandBlockRecord& CommandBlockStore::mint(int64_t headStableId, uint64_t headIdGeneration)
{
    // 2^32 prompts is out of reach of any session a person sits at, but not of a hostile stream replaying
    // OSC 133;A as fast as the pty delivers it. Starting over keeps the ids ascending, which find() relies
    // on; rows stamped before the wrap may then name a newer block -- cosmetic, and only on that path.
    if (_nextId == std::numeric_limits<uint32_t>::max())
    {
        _records.clear();
        _runningSince.reset();
        _nextId = 1;
    }

    auto& record = _records.emplace_back();
    record.id = CommandBlockId { _nextId };
    ++_nextId;
    record.promptStartedAt = _wallClock.now();
    record.headStableId = headStableId;
    record.headIdGeneration = headIdGeneration;
    evictOverflow();
    return _records.back();
}

void CommandBlockStore::evictOverflow() noexcept
{
    if (_records.size() <= _limits.maxRecords)
        return;
    auto const excess = static_cast<std::ptrdiff_t>(_records.size() - _limits.maxRecords);
    _records.erase(_records.begin(), _records.begin() + excess);
}
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[blockstore]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/vtbackend/shell/CommandBlock.hpp src/vtbackend/shell/CommandBlock.cpp src/vtbackend/shell/CommandBlock_test.cpp`

```bash
git add src/vtbackend/shell/CommandBlock.hpp src/vtbackend/shell/CommandBlock.cpp src/vtbackend/shell/CommandBlock_test.cpp
git commit -F - <<'EOF'
vtbackend: record command blocks in a bounded, clock-injected store

The OSC 133 transition table of the semantic-blocks spec (section 4.3): an
unstarted prompt is discarded by the next, a running block is closed
implicitly by a nested prompt, ;C without ;A mints a record, ;D with nothing
running is ignored. Records are capped, oldest first; 100k cycles stay
within the cap.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.3: CommandBlockStore — enrichment, head positions, adoption

**Files:**
- Modify: `src/vtbackend/shell/CommandBlock.hpp` (class `CommandBlockStore`: public section, `forEachRecord` doc)
- Modify: `src/vtbackend/shell/CommandBlock.cpp` (append)
- Test: `src/vtbackend/shell/CommandBlock_test.cpp` (append)

**Interfaces:**
- Consumes: Task 1.2.
- Produces (C1): `void enrichOutcome(ContextOutcome outcome)`, `void updateHeadPosition(CommandBlockId id, int64_t headStableId, uint64_t headIdGeneration)`, `[[nodiscard]] std::optional<CommandBlockSummary> adopt(CommandBlockRecord record, AdoptMode mode)`.

- [ ] **Step 1: Write the failing tests** — append to `src/vtbackend/shell/CommandBlock_test.cpp`:

```cpp
// {{{ enrichment, head positions, adoption

TEST_CASE("CommandBlockStore.enrichOutcomeRecordsHowACommandDiedWithoutClosingIt", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const killed = ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv };

    SECTION("before ;D: the running block keeps it through the finish")
    {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reported("crash"));
        f.store.enrichOutcome(killed);
        REQUIRE(f.store.current() != nullptr);
        CHECK(f.store.current()->state == CommandBlockState::Running); // enriched, not closed

        auto const summary = f.store.commandFinished(139);
        REQUIRE(summary.has_value());
        CHECK(summary->outcome == killed);
        CHECK(summary->exitCode == 139);
    }

    SECTION("after ;D, even after the next prompt: the finished block gains it")
    {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reported("crash"));
        std::ignore = f.store.commandFinished(139);
        f.store.promptStarted(1, 0);
        f.store.enrichOutcome(killed);

        auto const* finished = f.store.lastFinished();
        REQUIRE(finished != nullptr);
        CHECK(finished->outcome == killed);
        CHECK(finished->exitCode == 139);
        CHECK(finished->end == CommandBlockEnd::Reported);
    }

    SECTION("an outcome already recorded is kept")
    {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reported("crash"));
        std::ignore = f.store.commandFinished(130, ContextOutcome { .exit = ContextExit::Interrupt });
        f.store.enrichOutcome(killed);
        REQUIRE(f.store.lastFinished() != nullptr);
        CHECK(f.store.lastFinished()->outcome.exit == ContextExit::Interrupt);
    }

    SECTION("nothing to enrich changes nothing")
    {
        f.store.enrichOutcome(killed); // an empty store
        f.store.promptStarted(0, 0);
        auto const prompting = f.store.revision();
        f.store.enrichOutcome(killed);             // a prompt has no command that could have died
        f.store.enrichOutcome(ContextOutcome {}); // and an end= that says nothing adds nothing
        CHECK(f.store.revision() == prompting);
    }
}

TEST_CASE("CommandBlockStore.updateHeadPositionMovesOnlyWhatChanged", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const id = f.store.promptStarted(5, 1);
    auto const before = f.store.revision();

    f.store.updateHeadPosition(id, 5, 1);
    CHECK(f.store.revision() == before);

    f.store.updateHeadPosition(id, 12, 2);
    CHECK(f.store.revision() == before + 1);
    REQUIRE(f.store.find(id) != nullptr);
    CHECK(f.store.find(id)->headStableId == 12);
    CHECK(f.store.find(id)->headIdGeneration == 2);

    f.store.updateHeadPosition(CommandBlockId { 99 }, 1, 1);
    CHECK(f.store.revision() == before + 1);
}

TEST_CASE("CommandBlockStore.adoptTakesMirroredRecords", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto running = CommandBlockRecord {};
    running.id = CommandBlockId { 7 };
    running.state = CommandBlockState::Running;
    running.commandLine = "make";
    running.commandLineSource = CommandLineSource::Reported;

    SECTION("a snapshot brings history in silently")
    {
        auto finished = running;
        finished.state = CommandBlockState::Finished;
        finished.exitCode = 0;
        CHECK_FALSE(f.store.adopt(finished, AdoptMode::Snapshot).has_value());
        REQUIRE(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(*f.store.find(CommandBlockId { 7 }) == finished);
    }

    SECTION("a live delta announces the transition to Finished, once")
    {
        CHECK_FALSE(f.store.adopt(running, AdoptMode::Live).has_value());

        auto finished = running;
        finished.state = CommandBlockState::Finished;
        finished.exitCode = 2;
        finished.duration = 3s;
        auto const summary = f.store.adopt(finished, AdoptMode::Live);
        REQUIRE(summary.has_value());
        CHECK(summary->id == CommandBlockId { 7 });
        CHECK(summary->exitCode == 2);
        CHECK(summary->duration == 3s);

        // The same delta again -- a resend, a reattach -- announces nothing.
        CHECK_FALSE(f.store.adopt(finished, AdoptMode::Live).has_value());
    }

    SECTION("an implicit close is never announced")
    {
        auto superseded = running;
        superseded.state = CommandBlockState::Finished;
        superseded.end = CommandBlockEnd::Implicit;
        CHECK_FALSE(f.store.adopt(superseded, AdoptMode::Live).has_value());
    }

    SECTION("a record arriving out of order is still found, and its id is never minted again")
    {
        auto newer = running;
        newer.id = CommandBlockId { 9 };
        std::ignore = f.store.adopt(newer, AdoptMode::Snapshot);
        std::ignore = f.store.adopt(running, AdoptMode::Snapshot); // id 7, older

        auto order = std::vector<uint32_t> {};
        f.store.forEachRecord([&](CommandBlockRecord const& record) { order.push_back(unbox(record.id)); });
        CHECK(order == std::vector<uint32_t> { 7, 9 });
        CHECK(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(f.store.promptStarted(0, 0) == CommandBlockId { 10 });
    }

    SECTION("an id of zero is refused")
    {
        auto nameless = running;
        nameless.id = CommandBlockId {};
        CHECK_FALSE(f.store.adopt(nameless, AdoptMode::Live).has_value());
        CHECK(f.store.size() == 0);
    }

    SECTION("a mirrored command line is held to the same bound")
    {
        auto huge = running;
        huge.commandLine = std::string(MaxRecordedCommandLineBytes * 2, 'x');
        std::ignore = f.store.adopt(huge, AdoptMode::Snapshot);
        REQUIRE(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(f.store.find(CommandBlockId { 7 })->commandLine.size() <= MaxRecordedCommandLineBytes);
    }
}

// }}}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'enrichOutcome': is not a member of 'vtbackend::CommandBlockStore'`.

- [ ] **Step 3: Declare the three operations** — in `CommandBlockStore`'s public section, after `commandFinished`:

```cpp
    /// An OSC 3008 `end=` arrived while OSC 133 owns the marks: records HOW the command ended (a signal,
    /// a crash), which ;D's bare number cannot say. Never closes, re-closes or announces a block. The
    /// match is the running block, else the newest finished one; a block that already carries an outcome
    /// keeps it.
    /// @param outcome The ENDFIELDs OSC 3008 reported; a default outcome changes nothing.
    void enrichOutcome(ContextOutcome outcome);

    /// Re-records where a block's head row is, after a reflow destroyed stable row identity.
    ///
    /// Unknown ids are ignored. Safe to call from inside forEachRecord(): it never adds or removes a
    /// record.
    /// @param id The block.
    /// @param headStableId The head row's stable id, or one below the grid's floor when it was evicted.
    /// @param headIdGeneration The Grid::stableIdGeneration() that id is valid in.
    void updateHeadPosition(CommandBlockId id, int64_t headStableId, uint64_t headIdGeneration);

    /// Adopts a record replicated from a daemon host, keeping the host's id.
    ///
    /// Snapshot: an attach brings history in and announces nothing. Live: a delta, which announces a
    /// block's FIRST transition to a reported Finished -- so a reattach never replays finishes that
    /// happened while detached, and a repeated delta never announces twice.
    /// @param record The host's record; a zero id is refused.
    /// @param mode How it arrived.
    /// @return The finish to announce, or nullopt.
    [[nodiscard]] std::optional<CommandBlockSummary> adopt(CommandBlockRecord record, AdoptMode mode);
```

and replace the doc comment of `forEachRecord` with:

```cpp
    /// Visits every record, oldest first.
    ///
    /// The visitor may call updateHeadPosition() -- Terminal re-heads records this way after a reflow --
    /// but no other mutator.
    /// @param fn Called as fn(CommandBlockRecord const&).
```

- [ ] **Step 4: Implement them** — append to `src/vtbackend/shell/CommandBlock.cpp`, before `} // namespace vtbackend`:

```cpp
void CommandBlockStore::enrichOutcome(ContextOutcome outcome)
{
    if (outcome == ContextOutcome {})
        return;

    auto* target = unfinished();
    if (target && target->state != CommandBlockState::Running)
        target = nullptr; // a prompt has no command that could have ended
    if (!target)
        target = newestFinishedIn(_records);
    if (!target || target->outcome != ContextOutcome {})
        return;

    target->outcome = outcome;
    ++_revision;
}

void CommandBlockStore::updateHeadPosition(CommandBlockId id, int64_t headStableId, uint64_t headIdGeneration)
{
    auto const it = positionOf(_records, id);
    if (it == _records.end()
        || (it->headStableId == headStableId && it->headIdGeneration == headIdGeneration))
        return;

    it->headStableId = headStableId;
    it->headIdGeneration = headIdGeneration;
    ++_revision;
}

std::optional<CommandBlockSummary> CommandBlockStore::adopt(CommandBlockRecord record, AdoptMode mode)
{
    if (!record.id)
        return std::nullopt;

    auto const id = record.id;
    record.commandLine = truncatedCommandLine(record.commandLine);
    auto const announcesFinish = mode == AdoptMode::Live && record.state == CommandBlockState::Finished
                                 && record.end == CommandBlockEnd::Reported;

    auto wasFinished = false;
    if (auto const it = positionOf(_records, id); it != _records.end())
    {
        if (*it == record)
            return std::nullopt;
        wasFinished = it->state == CommandBlockState::Finished;
        *it = std::move(record);
    }
    else
    {
        // Ids ascend, so the record goes where its id sorts -- usually the back.
        _records.insert(std::ranges::upper_bound(_records, id, std::less {}, &CommandBlockRecord::id),
                        std::move(record));
        evictOverflow();
    }

    // A mirror mints nothing itself; were it ever to, it must not reuse an id the host handed it.
    if (unbox(id) >= _nextId && unbox(id) != std::numeric_limits<uint32_t>::max())
        _nextId = unbox(id) + 1;
    ++_revision;

    auto const* stored = find(id);
    if (!announcesFinish || wasFinished || !stored)
        return std::nullopt;
    return summaryOf(*stored);
}
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[blockstore]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/vtbackend/shell/CommandBlock.hpp src/vtbackend/shell/CommandBlock.cpp src/vtbackend/shell/CommandBlock_test.cpp`

```bash
git add src/vtbackend/shell/CommandBlock.hpp src/vtbackend/shell/CommandBlock.cpp src/vtbackend/shell/CommandBlock_test.cpp
git commit -F - <<'EOF'
vtbackend: let the block store enrich, re-head and adopt records

enrichOutcome() takes OSC 3008's signal names while OSC 133 owns the marks,
updateHeadPosition() is the rescan seam for a destroyed row identity, and
adopt() is the daemon mirror's way in: Snapshot announces nothing, Live
announces a block's first reported finish exactly once.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.4: `Line` carries a block id and a birth time

**Files:**
- Modify: `src/vtbackend/grid/Line.hpp` (includes L4-25; `operator=` L117-145; `reset`/`fill` L177-224; accessors after `adoptContext` L415-419; members and `static_assert` L636-678)
- Test: `src/vtbackend/grid/Line_test.cpp` (append)

**Interfaces:**
- Consumes: `CommandBlockId` (`core/CommandBlockId.hpp`, Task 1.1).
- Produces (C1): `[[nodiscard]] CommandBlockId blockId() const noexcept`, `void adoptBlock(CommandBlockId id) noexcept`, `[[nodiscard]] uint32_t bornAt() const noexcept`, `void stampBornAt(uint32_t secondsPlusOne) noexcept`, `static constexpr uint32_t MaxBornAt = (1U << 24U) - 1U`. Tail layout `+24 bool _dirty | +25..27 std::array<uint8_t, 3> _bornAt | +28..31 CommandBlockId _blockId`; `sizeof(Line) == sizeof(LineSoA) + 32` keeps holding.

- [ ] **Step 1: Write the failing tests** — append to `src/vtbackend/grid/Line_test.cpp` (add `#include <utility>` to its includes):

```cpp
// {{{ block id and birth time

TEST_CASE("Line.block.adoptIgnoresZeroAndLeavesTheLineClean", "[Line][semanticblocks]")
{
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    static_cast<void>(line.stampRevision(1)); // clean

    CHECK(line.blockId() == CommandBlockId {});
    line.adoptBlock(CommandBlockId { 4 });
    CHECK(line.blockId() == CommandBlockId { 4 });
    // Derived state, like the context id: adopting it is not a change a mirror must be told about.
    CHECK_FALSE(line.isDirty());

    // Zero is an absence of information, not a claim: it never erases an id the line already has.
    line.adoptBlock(CommandBlockId {});
    CHECK(line.blockId() == CommandBlockId { 4 });

    // Last writer wins: the row a ;D closes is routinely the row the next ;A opens.
    line.adoptBlock(CommandBlockId { 5 });
    CHECK(line.blockId() == CommandBlockId { 5 });
}

TEST_CASE("Line.bornAt.stampsOnceAndDirtiesOnce", "[Line][semanticblocks]")
{
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    static_cast<void>(line.stampRevision(1));
    CHECK(line.bornAt() == 0);

    line.stampBornAt(0); // "no clock yet" stamps nothing
    CHECK(line.bornAt() == 0);
    CHECK_FALSE(line.isDirty());

    line.stampBornAt(0x12'34'56);
    CHECK(line.bornAt() == 0x12'34'56); // all three bytes round-trip
    CHECK(line.isDirty());              // so the daemon's delta carries the stamp

    static_cast<void>(line.stampRevision(2));
    line.stampBornAt(99); // the cursor coming back later does not make the line younger
    CHECK(line.bornAt() == 0x12'34'56);
    CHECK_FALSE(line.isDirty());
}

TEST_CASE("Line.bornAt.saturatesAtTheHorizon", "[Line][semanticblocks]")
{
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    line.stampBornAt(Line::MaxBornAt + 1000);
    CHECK(line.bornAt() == Line::MaxBornAt);
}

TEST_CASE("Line.blockAndBirth.areClearedWhenTheLineIsReused", "[Line][semanticblocks]")
{
    auto const stamped = [] {
        auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
        line.adoptBlock(CommandBlockId { 3 });
        line.stampBornAt(7);
        return line;
    };

    SECTION("reset")
    {
        auto line = stamped();
        line.reset(LineFlag::None, GraphicsAttributes {});
        CHECK(line.blockId() == CommandBlockId {});
        CHECK(line.bornAt() == 0);
    }

    SECTION("reset to a new width")
    {
        auto line = stamped();
        line.reset(LineFlag::None, GraphicsAttributes {}, ColumnCount(10));
        CHECK(line.blockId() == CommandBlockId {});
        CHECK(line.bornAt() == 0);
    }

    SECTION("fill")
    {
        auto line = stamped();
        line.fill(LineFlag::None, GraphicsAttributes {}, U'E', 1);
        CHECK(line.blockId() == CommandBlockId {});
        CHECK(line.bornAt() == 0);
    }
}

TEST_CASE("Line.blockAndBirth.travelWithTheLineOnAssignment", "[Line][semanticblocks]")
{
    // Margin scrolls move whole lines between rows by assignment: the row that receives the text must
    // receive its block and its birth time with it.
    auto source = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    source.adoptBlock(CommandBlockId { 9 });
    source.stampBornAt(11);

    auto copied = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    copied = source;
    CHECK(copied.blockId() == CommandBlockId { 9 });
    CHECK(copied.bornAt() == 11);

    auto moved = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    moved = std::move(source);
    CHECK(moved.blockId() == CommandBlockId { 9 });
    CHECK(moved.bornAt() == 11);
}

// }}}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'blockId': is not a member of 'vtbackend::Line'`.

- [ ] **Step 3: Add the includes** — in `src/vtbackend/grid/Line.hpp`:

```cpp
#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/ContextId.hpp>
```

and in the standard-library group:

```cpp
#include <algorithm>
#include <array>
#include <cstdint>
```

- [ ] **Step 4: Carry them through assignment** — in both `operator=(Line const&)` and `operator=(Line&&)`, replace

```cpp
        _revision = other._revision;
        _dirty = true;
        return *this;
```

with

```cpp
        _revision = other._revision;
        _bornAt = other._bornAt;
        _blockId = other._blockId;
        _dirty = true;
        return *this;
```

- [ ] **Step 5: Clear them when the line is reused** — in `reset(LineFlags, GraphicsAttributes)`, `reset(LineFlags, GraphicsAttributes, ColumnCount)` and `fill(LineFlags, GraphicsAttributes const&, char32_t, uint8_t)`, each of which contains

```cpp
        _contextId = {};
        _commandEndOffset = {};
        _promptEndOffset = {};
```

replace that run (three times) with

```cpp
        _contextId = {};
        _blockId = {};
        _bornAt = {};
        _commandEndOffset = {};
        _promptEndOffset = {};
```

- [ ] **Step 6: Add the accessors** — directly after `adoptContext()` (ends `_contextId = id; }`), insert:

```cpp
    /// The shell command block this line belongs to, or zero when none. @see CommandBlockStore.
    [[nodiscard]] CommandBlockId blockId() const noexcept { return _blockId; }

    /// Records that this line belongs to the command block @p id.
    ///
    /// The contract of adoptContext(), for the same reasons: a ZERO id is ignored (no block being active is
    /// an absence of information, not a claim that the line has none), and adopting does NOT dirty the line
    /// -- the id is derived state, carried to a mirror with the line's next real change and in snapshots.
    /// Last writer wins: the line a ;D closes is routinely the line the next ;A opens.
    void adoptBlock(CommandBlockId id) noexcept
    {
        if (!!id)
            _blockId = id;
    }

    /// When the cursor first reached this line: seconds since the session began, PLUS ONE, so that zero
    /// can mean "never stamped". Saturates at MaxBornAt, past which the time is no longer known.
    [[nodiscard]] uint32_t bornAt() const noexcept
    {
        return static_cast<uint32_t>(_bornAt[0]) | (static_cast<uint32_t>(_bornAt[1]) << 8U)
               | (static_cast<uint32_t>(_bornAt[2]) << 16U);
    }

    /// Stamps the time the cursor first reached this line, if it carries none yet.
    ///
    /// Unlike adoptBlock(), this DIRTIES the line -- once, the moment it is first reached -- so the
    /// daemon's delta carries the stamp (and the block id adopted with it) even for a line nothing is
    /// ever written to, such as a blank line in a command's output.
    /// @param secondsPlusOne Seconds since the session began, plus one. Zero stamps nothing; a value past
    ///        MaxBornAt is stored as MaxBornAt.
    void stampBornAt(uint32_t secondsPlusOne) noexcept
    {
        if (secondsPlusOne == 0 || bornAt() != 0)
            return;
        auto const value = std::min(secondsPlusOne, MaxBornAt);
        _bornAt = { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U), static_cast<uint8_t>(value >> 16U) };
        _dirty = true;
    }

    /// The largest birth stamp three bytes hold: 2^24 - 1 seconds, about 194 days of uptime.
    static constexpr uint32_t MaxBornAt = (1U << 24U) - 1U;
```

- [ ] **Step 7: Add the members into the padding** — replace

```cpp
    bool _dirty = true;     ///< Fresh lines are pending: bootstrap self-heals.
};
```

with

```cpp
    bool _dirty = true;     ///< Fresh lines are pending: bootstrap self-heals.

    /// The birth stamp (@see bornAt), three bytes little-endian. Placed in the padding after _dirty, like
    /// _blockId below, so neither costs a byte per line; the static_assert after the class keeps it so.
    std::array<uint8_t, 3> _bornAt {};

    /// The command block this line belongs to (@see blockId). NOT head-only, for the reason _contextId
    /// gives: it names what produced the line, and a wrap does not change that.
    CommandBlockId _blockId {};
};
```

and extend the comment above the `static_assert` with one paragraph (the assertion itself is unchanged):

```cpp
/// The tail after LineSoA is, by offset: _columns (+0), _flags (+4), _contextId (+6), _commandEndOffset
/// (+8), _promptEndOffset (+12), _revision (+16), _dirty (+24), _bornAt (+25..27), _blockId (+28..31) --
/// every byte used, none added. A field that does not fit there is a decision to grow every line.
```

- [ ] **Step 8: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[Line]"`
Expected: zero warnings (the `static_assert(sizeof(Line) == sizeof(LineSoA) + 32)` still compiles); `All tests passed`.

- [ ] **Step 9: Format and commit**

Run: `clang-format -i src/vtbackend/grid/Line.hpp src/vtbackend/grid/Line_test.cpp`

```bash
git add src/vtbackend/grid/Line.hpp src/vtbackend/grid/Line_test.cpp
git commit -F - <<'EOF'
vtbackend: give every line a block id and a birth time

Both live in the seven padding bytes after Line::_dirty, so the tail stays
32 bytes. Adopting a block id does not dirty the line; the first birth
stamp does, once. reset() and fill() clear both, assignment carries both.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.5: Reflow carries block ids and birth times on every chunk

**Files:**
- Modify: `src/vtbackend/grid/Grid.cpp` (`LogicalLineAttributes` L48-73; `emitChunk` L106-108; grow path L942-948; shrink path L1022-1028, L1053-1063, L1076-1077, L1085)
- Test: `src/vtbackend/grid/Grid_test.cpp` (append after `Grid.reflow.semanticMarksStayOnTheHeadLine`, ~L1466)

**Interfaces:**
- Consumes: Task 1.4 (`Line::blockId/adoptBlock/bornAt/stampBornAt`).
- Produces: internal `detail::carriedBy(Line const&, LineFlags) noexcept` and `detail::applyCarried(Line&, LogicalLineAttributes const&) noexcept` (Grid.cpp only). Reflow rule: a chunk created by a split carries the attributes of the row it overflowed from; a continuation rebuilt in place keeps its own (the existing `ContextId` rule); a widening rebuild gives every chunk the head's.

- [ ] **Step 1: Write the failing test** — add `#include <algorithm>` to the includes of `src/vtbackend/grid/Grid_test.cpp`, and insert after the closing `}` of `Grid.reflow.semanticMarksStayOnTheHeadLine`:

```cpp
TEST_CASE("Grid.reflow.blockIdAndBirthTimeRideOnEveryChunk", "[grid][semanticblocks]")
{
    // Unlike the semantic marks above, a row's block and birth time are NOT head-only: they say which
    // command produced the row and when, and re-chopping a logical line into different physical pieces
    // changes neither. Losing them would erase a running command's colour and timestamps the moment the
    // window is resized.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(30) }, true, LineCount(10));
    grid.setLineText(LineOffset(0), std::string(30, 'A'));
    grid.lineAt(LineOffset(0)).adoptBlock(CommandBlockId { 7 });
    grid.lineAt(LineOffset(0)).stampBornAt(42);
    grid.setLineText(LineOffset(1), "BBBB");
    grid.lineAt(LineOffset(1)).adoptBlock(CommandBlockId { 8 });
    grid.lineAt(LineOffset(1)).stampBornAt(50);

    // Every non-blank row, top first: its first character, its block and its birth time.
    auto const rows = [](Grid const& g) {
        auto result = std::vector<std::tuple<char, CommandBlockId, uint32_t>> {};
        for (auto const i: std::views::iota(-g.historyLineCount().as<int>(), g.pageSize().lines.as<int>()))
        {
            auto const offset = LineOffset::cast_from(i);
            auto const text = g.lineTextTrimmed(offset);
            if (!text.empty())
                result.emplace_back(text.front(), g.lineAt(offset).blockId(), g.lineAt(offset).bornAt());
        }
        return result;
    };
    auto const everyRowKeepsItsOwner = [&](Grid const& g) {
        auto const all = rows(g);
        REQUIRE_FALSE(all.empty());
        for (auto const& [first, block, bornAt]: all)
        {
            INFO("row starting with " << first);
            CHECK(block == (first == 'A' ? CommandBlockId { 7 } : CommandBlockId { 8 }));
            CHECK(bornAt == (first == 'A' ? 42U : 50U));
        }
    };

    SECTION("narrowing splits the line into chunks that all keep it")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, false);
        CHECK(std::ranges::count(rows(grid), 'A', [](auto const& row) { return std::get<0>(row); }) == 3);
        everyRowKeepsItsOwner(grid);
    }

    SECTION("narrowing twice rebuilds continuations that still keep it")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, false);
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(7) }, CellLocation {}, false);
        everyRowKeepsItsOwner(grid);
    }

    SECTION("widening rejoins the chunks without losing it")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, false);
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(20) }, CellLocation {}, false);
        everyRowKeepsItsOwner(grid);
    }
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "Grid.reflow.blockIdAndBirthTimeRideOnEveryChunk"`
Expected: FAIL — the continuation rows report `block == {0}` and `bornAt == 0`.

- [ ] **Step 3: Extend `LogicalLineAttributes` and add the two helpers** — in `src/vtbackend/grid/Grid.cpp`, replace

```cpp
            /// NOT head-only, unlike the two above: those name a border WITHIN the logical line, while this
            /// names who WROTE it, and a wrap does not change the author of a continuation.
            ContextId contextId {};
        };
    } // namespace
```

with

```cpp
            /// NOT head-only, unlike the two above: those name a border WITHIN the logical line, while this
            /// names who WROTE it, and a wrap does not change the author of a continuation.
            ContextId contextId {};

            /// The command block the line belongs to. Not head-only, for the reason contextId gives.
            CommandBlockId blockId {};

            /// When the cursor first reached the line (@see Line::bornAt). Not head-only either.
            uint32_t bornAt = 0;
        };

        /// What every physical piece of @p line's logical line inherits from it, whatever its position.
        /// The two head-only offsets are left for the caller, who alone knows whether a head is emitted.
        /// @param line The row the attributes are read from.
        /// @param flags The flags the pieces start from.
        [[nodiscard]] LogicalLineAttributes carriedBy(Line const& line, LineFlags flags) noexcept
        {
            return LogicalLineAttributes { .flags = flags,
                                           .contextId = line.contextId(),
                                           .blockId = line.blockId(),
                                           .bornAt = line.bornAt() };
        }

        /// Stamps the per-piece attributes onto @p line: the single place that knows what they are.
        void applyCarried(Line& line, LogicalLineAttributes const& attributes) noexcept
        {
            line.adoptContext(attributes.contextId);
            line.adoptBlock(attributes.blockId);
            line.stampBornAt(attributes.bornAt);
        }
    } // namespace
```

- [ ] **Step 4: Use them at every site**

In `addNewWrappedLines`' `emitChunk`, replace

```cpp
            // EVERY chunk, head or not. @see LogicalLineAttributes::contextId.
            targetLines.back().adoptContext(attributes.contextId);
```

with

```cpp
            // EVERY chunk, head or not. @see LogicalLineAttributes::contextId.
            applyCarried(targetLines.back(), attributes);
```

In `growColumns`, replace

```cpp
                        logicalLineAttributes = {
                            .flags = line.flags().without(LineFlag::Wrapped),
                            .commandEndOffset = line.commandEndOffset(),
                            .promptEndOffset = line.promptEndOffset(),
                            .contextId = line.contextId(),
                        };
```

with

```cpp
                        logicalLineAttributes = detail::carriedBy(line, line.flags().without(LineFlag::Wrapped));
                        logicalLineAttributes.commandEndOffset = line.commandEndOffset();
                        logicalLineAttributes.promptEndOffset = line.promptEndOffset();
```

In `shrinkColumns`, replace

```cpp
            auto previousAttributes =
                detail::LogicalLineAttributes { .flags = _lines.front().inheritableFlags(),
                                                .contextId = _lines.front().contextId() };
```

with

```cpp
            auto previousAttributes = detail::carriedBy(_lines.front(), _lines.front().inheritableFlags());
```

replace

```cpp
                        // Carried across the rebuild explicitly. Unlike the offsets above, the context
                        // is NOT default-correct here: this line and the columns being prepended to it
                        // are two physical pieces of ONE logical line, so they share an author, and a
                        // fresh Line would silently report none.
                        auto const carriedContext = line.contextId();
                        line = Line(line.flags(), std::move(merged), ColumnCount::cast_from(totalCols));
                        line.adoptContext(carriedContext);
```

with

```cpp
                        // Carried across the rebuild explicitly. Unlike the offsets above, the context,
                        // block and birth time are NOT default-correct here: this line and the columns
                        // being prepended to it are two physical pieces of ONE logical line, so they share
                        // an author and a block, and a fresh Line would silently report none.
                        auto const carried = detail::carriedBy(line, line.flags());
                        line = Line(line.flags(), std::move(merged), ColumnCount::cast_from(totalCols));
                        detail::applyCarried(line, carried);
```

replace

```cpp
                        previousAttributes = { .flags = line.inheritableFlags(),
                                               .contextId = line.contextId() };
```

with

```cpp
                        previousAttributes = detail::carriedBy(line, line.inheritableFlags());
```

and replace

```cpp
                    previousAttributes = { .flags = line.inheritableFlags(), .contextId = line.contextId() };
```

with

```cpp
                    previousAttributes = detail::carriedBy(line, line.inheritableFlags());
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[grid]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[context]"`
Expected: zero warnings; `All tests passed` for both (the OSC 3008 reflow cases prove the context carry is unchanged).

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/vtbackend/grid/Grid.cpp src/vtbackend/grid/Grid_test.cpp`

```bash
git add src/vtbackend/grid/Grid.cpp src/vtbackend/grid/Grid_test.cpp
git commit -F - <<'EOF'
vtbackend: carry block ids and birth times through reflow

LogicalLineAttributes gains both, and the context, block and birth time are
now read and applied in one place each (carriedBy/applyCarried) instead of
at six sites, so a column resize no longer drops a row's block or time.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.6: `LineFlag::UserMark` — vi `mm` is a bookmark, not a prompt

**Files:**
- Modify: `src/vtbackend/core/LineFlags.hpp` (table L18-34; `HeadOnlyLineFlags` L62-70)
- Modify: `src/vthost/GridWire.cpp` (`PinnedLineFlagBits` L30-40) — the wire pin's count assertion fails the build otherwise
- Modify: `src/vtbackend/input/vi/ViCommands.cpp` (`toggleLineMark` L293-303; `TextObject::LineMark` L648-660; `LineMarkUp`/`LineMarkDown` L973-1005)
- Modify: `src/vtbackend/screen/Screen.cpp` (`findMarkerUpwards` L995-1010, `findMarkerDownwards` L1012-1028)
- Modify: `src/vtbackend/shell/PromptRegion.cpp` (L23)
- Modify: `src/vtbackend/grid/Grid.hpp` (`clampHistory` doc L1019-1041 — two phrases)
- Modify: `docs/demo/line-marks.md`
- Test: `src/vtbackend/input/vi/ViCommands_test.cpp` (L469-494 and append), `src/vtbackend/screen/Screen_test.cpp` (after `findMarkerUpwards`, ~L3534), `src/vtbackend/grid/Grid_test.cpp` (after `Grid.historyEviction.withoutSemanticMarksBehavesExactlyAsBefore`, ~L2191), `src/vtbackend/shell/Folding_test.cpp` (after `Folding.terminal.rangesComeFromTheMarks`, ~L665), `src/vtbackend/grid/Line_test.cpp` (append)

**Interfaces:**
- Consumes: nothing new.
- Produces (C1): `LineFlag::UserMark` (bit 9, in `HeadOnlyLineFlags`); `constexpr inline auto NavigationMarkFlags = LineFlags { LineFlag::Marked, LineFlag::UserMark };`.

**Every consumer of `LineFlag::Marked` / `Line::marked()` and whether it sees `UserMark`:**

| Consumer | Sees UserMark? | Why |
|---|---|---|
| `ViCommands::toggleLineMark` (`mm`) | writes it | the bookmark itself |
| `ViCommands` `[m` / `]m` (`LineMarkUp/Down`) | yes | navigation (§4.6) |
| `ViCommands` `im` / `am` (`TextObject::LineMark`) | yes | navigation (§4.6) |
| `Screen::findMarkerUpwards/Downwards` → `Viewport::scrollMarkUp/Down`, `Terminal::extractLastMarkRange` | yes | ScrollMarkUp/Down and CopyPreviousMarkRange are navigation; `mm` fed them before |
| `Grid::clampHistory` (`lineAt(y).marked()`) | no | block-atomic eviction cuts at prompts only (§4.6) |
| `computeFoldRanges` (`Folding.cpp` L117, L128) | no | a fold hangs off a prompt |
| `scanCommandBlocksBackward` (`CommandBlocks.cpp` L120, L137) | no | "copy last command" reads prompts |
| `findLivePromptRegion` (`PromptRegion.cpp` L41) | no | the live prompt |
| `findLivePromptRegion` "any mark" probe (L23) | no | a bookmark is not shell integration |
| `Screen::handleInProgressQuery` (`Screen.cpp` L6506) | no | mode 2034 prompt text |
| `Screen::setMark` (OSC 133;A, OSC 3008) | writes `Marked` | a prompt by definition |
| `GridWire.cpp` pin table | carries both | wire encoding is verbatim |

- [ ] **Step 1: Write the failing tests**

In `src/vtbackend/input/vi/ViCommands_test.cpp`, replace the body of `vi.mark: \`mm\` marks the LOGICAL line, not the wrapped piece the cursor sits in` from `mock.sendCharEvent(U'j');` to the end of the test with:

```cpp
    mock.sendCharEvent(U'j'); // onto the first continuation
    mock.sendCharSequence("mm");

    CHECK(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::UserMark));
    CHECK_FALSE(screen.isLineFlagEnabledAt(vtbackend::LineOffset(1), vtbackend::LineFlag::UserMark));
    // A bookmark, not a prompt: block-atomic eviction and folding cut at Marked, and must not cut here.
    CHECK_FALSE(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::Marked));

    SECTION("and toggles it off again from that same continuation")
    {
        mock.sendCharSequence("mm");
        CHECK_FALSE(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::UserMark));
    }
}
```

and append, before the trailing `// }}}` of that fold:

```cpp
TEST_CASE("vi.mark: `[m` and `]m` visit user marks and prompt marks alike", "[vi][semanticblocks]")
{
    auto mock = setupMockTerminal("0\r\n1\r\n2\r\n3\r\n4\r\n5");
    auto& screen = mock.terminal.currentScreen();
    screen.setLogicalLineFlags(vtbackend::LineOffset(1), vtbackend::LineFlag::Marked, true);   // a prompt
    screen.setLogicalLineFlags(vtbackend::LineOffset(3), vtbackend::LineFlag::UserMark, true); // `mm`

    mock.sendCharSequence("5j");
    REQUIRE(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(5));

    mock.sendCharSequence("[m");
    CHECK(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(3));
    mock.sendCharSequence("[m");
    CHECK(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(1));
    mock.sendCharSequence("]m");
    CHECK(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(3));
}

TEST_CASE("vi.mark: `vim` spans between a prompt mark and a user mark", "[vi][semanticblocks]")
{
    auto mock = setupMockTerminal("0\r\n1\r\n2\r\n3\r\n4\r\n5");
    auto& screen = mock.terminal.currentScreen();
    screen.setLogicalLineFlags(vtbackend::LineOffset(1), vtbackend::LineFlag::Marked, true);
    screen.setLogicalLineFlags(vtbackend::LineOffset(3), vtbackend::LineFlag::UserMark, true);

    mock.sendCharSequence("2j");
    mock.sendCharSequence("vim");

    REQUIRE(mock.terminal.selector() != nullptr);
    CHECK(mock.terminal.selector()->from().line == vtbackend::LineOffset(2));
    CHECK(mock.terminal.selector()->to().line == vtbackend::LineOffset(2));
}
```

In `src/vtbackend/screen/Screen_test.cpp`, after the closing `}` of `TEST_CASE("findMarkerUpwards", "[screen]")`:

```cpp
TEST_CASE("findMarker visits user marks as well as prompt marks", "[screen][semanticblocks]")
{
    // ScrollMarkUp/ScrollMarkDown and CopyPreviousMarkRange walk these: `mm` fed them before it became a
    // user mark, and must go on feeding them.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(4) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("1abc\r\n2def\r\n3ghi\r\n4jkl\r\n5mno\r\n6pqr");
    REQUIRE(screen.historyLineCount() == LineCount { 3 });

    screen.setLogicalLineFlags(LineOffset(-2), LineFlag::UserMark, true); // 2def

    auto const up = screen.findMarkerUpwards(LineOffset(0));
    REQUIRE(up.has_value());
    CHECK(*up == LineOffset(-2));

    auto const down = screen.findMarkerDownwards(LineOffset(-3));
    REQUIRE(down.has_value());
    CHECK(*down == LineOffset(-2));
}
```

In `src/vtbackend/grid/Grid_test.cpp`, after the closing `}` of `Grid.historyEviction.withoutSemanticMarksBehavesExactlyAsBefore`:

```cpp
TEST_CASE("Grid.historyEviction.aUserMarkIsNotABlockStart", "[grid][history-eviction][semanticblocks]")
{
    // Vi's `mm` used to stamp LineFlag::Marked, so a bookmark dropped mid-output became a place where
    // block-atomic eviction would cut. With nothing but user marks in the scrollback, eviction must behave
    // exactly as with no marks at all.
    auto withUserMarks = makeGrid(LineCount(1), LineCount(4), LineCount(8));
    auto plain = Grid(PageSize { LineCount(1), ColumnCount(5) }, false, LineCount(8));

    for (auto const i: std::views::iota(0, 30))
    {
        appendLine(withUserMarks, 'L', i);
        withUserMarks.lineAt(LineOffset(0)).setFlag(LineFlag::UserMark, i % 3 == 0);
        appendLine(plain, 'L', i);
    }

    CHECK(withUserMarks.historyLineCount() == plain.historyLineCount());
    CHECK(historyText(withUserMarks) == historyText(plain));
}
```

In `src/vtbackend/shell/Folding_test.cpp`, after the closing `}` of `Folding.terminal.rangesComeFromTheMarks`:

```cpp
TEST_CASE("Folding.terminal.aUserMarkNeitherStartsNorSplitsAFold", "[folding][semanticblocks]")
{
    // `mm` on an output line: before UserMark existed this stamped LineFlag::Marked, and the fold scan read
    // it as a second prompt that split the block in two.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2", "file3" });
    mc.terminal.primaryScreen().setLogicalLineFlags(LineOffset(2), LineFlag::UserMark, true);

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    auto const& grid = mc.terminal.primaryScreen().grid();
    CHECK(ranges[0].headStableId == grid.stableLineIdOf(LineOffset(0)));
    CHECK(ranges[0].lastStableId == grid.stableLineIdOf(LineOffset(3)));
}
```

Append to `src/vtbackend/grid/Line_test.cpp` (add `#include <format>` to its includes):

```cpp
TEST_CASE("LineFlags.userMarkIsAHeadOnlyNavigationMark", "[Line][semanticblocks]")
{
    CHECK(std::format("{}", LineFlags { LineFlag::UserMark }) == "UserMark");
    CHECK(HeadOnlyLineFlags.contains(LineFlag::UserMark));
    CHECK(NavigationMarkFlags == LineFlags { LineFlag::Marked, LineFlag::UserMark });
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'UserMark': is not a member of 'vtbackend::LineFlag'`.

- [ ] **Step 3: Add the flag** — in `src/vtbackend/core/LineFlags.hpp`, replace

```cpp
    /* The shell's prompt finished printing and user input begins (OSC 133;B); the column it ended \ \ \
       at is Line::promptEndOffset(). */                                                               \
    _(PromptEnd, 8)
```

with

```cpp
    /* The shell's prompt finished printing and user input begins (OSC 133;B); the column it ended \ \ \
       at is Line::promptEndOffset(). */                                                               \
    _(PromptEnd, 8)                                                                                    \
    /* The user's own bookmark (vi `mm`): a navigation target, never a command-block boundary. */     \
    _(UserMark, 9)
```

and replace

```cpp
constexpr inline auto HeadOnlyLineFlags =
    LineFlags { LineFlag::Marked, LineFlag::OutputStart, LineFlag::CommandEnd, LineFlag::PromptEnd };
```

with

```cpp
constexpr inline auto HeadOnlyLineFlags = LineFlags {
    LineFlag::Marked, LineFlag::OutputStart, LineFlag::CommandEnd, LineFlag::PromptEnd, LineFlag::UserMark
};

/// The marks a user NAVIGATES between: prompts (OSC 133;A, OSC 3008's stand-in) and the user's own
/// bookmarks (vi `mm`). `[m`/`]m`, the `im`/`am` text objects, ScrollMarkUp/ScrollMarkDown and
/// CopyPreviousMarkRange read this; block STRUCTURE -- block-atomic eviction, folding, the command-block
/// and live-prompt scans -- reads LineFlag::Marked alone, so a bookmark never splits a command.
constexpr inline auto NavigationMarkFlags = LineFlags { LineFlag::Marked, LineFlag::UserMark };
```

- [ ] **Step 4: Pin its wire bit** — in `src/vthost/GridWire.cpp`, append to `PinnedLineFlagBits`:

```cpp
        std::pair { vtbackend::LineFlag::PromptEnd, 8 },
        std::pair { vtbackend::LineFlag::UserMark, 9 },
    };
```

(The protocol is unreleased and its codec version pinned — spec §12 — so adding a bit needs no `CodecVersion` bump.)

- [ ] **Step 5: Make `mm` set it and navigation visit both** — in `src/vtbackend/input/vi/ViCommands.cpp`, in `toggleLineMark()` replace

```cpp
    auto& screen = _terminal->currentScreen();
    auto const marked = screen.isLogicalLineFlagEnabled(cursorPosition.line, LineFlag::Marked);
    screen.setLogicalLineFlags(cursorPosition.line, LineFlag::Marked, !marked);
```

with

```cpp
    //
    // A USER mark, not a prompt mark: `mm` says "I want to come back here", not "a command starts here".
    // Stamping LineFlag::Marked made a bookmark count as a block start for block-atomic eviction and for
    // folding. Navigation visits both kinds (@see NavigationMarkFlags).
    auto& screen = _terminal->currentScreen();
    auto const marked = screen.isLogicalLineFlagEnabled(cursorPosition.line, LineFlag::UserMark);
    screen.setLogicalLineFlags(cursorPosition.line, LineFlag::UserMark, !marked);
```

in `translateToCellRange`'s `TextObject::LineMark` case replace

```cpp
            while (a.line > gridTop
                   && !(_terminal->currentScreen().lineFlagsAt(a.line).contains(LineFlag::Marked)))
                --a.line;
```

with

```cpp
            while (a.line > gridTop
                   && !_terminal->currentScreen().isLineFlagEnabledAt(a.line, NavigationMarkFlags))
                --a.line;
```

and

```cpp
            while (b.line < gridBottom
                   && !(_terminal->currentScreen().lineFlagsAt(b.line).contains(LineFlag::Marked)))
                ++b.line;
```

with

```cpp
            while (b.line < gridBottom
                   && !_terminal->currentScreen().isLineFlagEnabledAt(b.line, NavigationMarkFlags))
                ++b.line;
```

in `ViMotion::LineMarkUp` replace both `isLineFlagEnabledAt(result.line, LineFlag::Marked)` with `isLineFlagEnabledAt(result.line, NavigationMarkFlags)`, and in `ViMotion::LineMarkDown` replace

```cpp
                    if (_terminal->currentScreen().lineFlagsAt(result.line).contains(LineFlag::Marked))
                        break;
```

with

```cpp
                    if (_terminal->currentScreen().isLineFlagEnabledAt(result.line, NavigationMarkFlags))
                        break;
```

- [ ] **Step 6: Make the marker search visit both** — in `src/vtbackend/screen/Screen.cpp`, in both `findMarkerUpwards` and `findMarkerDownwards` replace

```cpp
        if (_grid.lineAt(i).marked())
            return { i };
```

with

```cpp
        if (_grid.lineAt(i).isFlagEnabled(NavigationMarkFlags))
            return { i };
```

- [ ] **Step 6b: Keep a bookmark from counting as shell integration** — in `src/vtbackend/shell/PromptRegion.cpp`, in `findLivePromptRegion`, replace

```cpp
        if ((marks.flags & HeadOnlyLineFlags).any())
            sawAnyMark = true;
```

with

```cpp
        if ((marks.flags & HeadOnlyLineFlags.without(LineFlag::UserMark)).any())
            sawAnyMark = true;
```

- [ ] **Step 7: Correct the eviction doc** — in `src/vtbackend/grid/Grid.hpp`, in the comment on `clampHistory()`, replace

```cpp
    /// A *block start* is a row carrying LineFlag::Marked -- what OSC 133;A stamps at a prompt (and
    /// what the OSC 3008 synthesis and Vi's `mm` stamp too). The rule is one sentence: drop
```

with

```cpp
    /// A *block start* is a row carrying LineFlag::Marked -- what OSC 133;A stamps at a prompt (and
    /// what the OSC 3008 synthesis stamps too; Vi's `mm` sets LineFlag::UserMark, which is no block
    /// start). The rule is one sentence: drop
```

and replace

```cpp
    /// history row that has already been passed -- Vi's `mm` deep in the scrollback, or a mirror
    /// replaying one. That boundary is then simply not used, which costs an eviction its snapping
```

with

```cpp
    /// history row that has already been passed -- a mirror replaying a prompt mark deep in the
    /// scrollback. That boundary is then simply not used, which costs an eviction its snapping
```

- [ ] **Step 8: Document it** — in `docs/demo/line-marks.md`, insert before `## Line marks as text objects`:

```markdown
## Marking a line yourself

In Vi-like normal mode, `mm` toggles a *user mark* on the line the cursor is on. A user mark is a
bookmark, not a prompt: `[m`/`]m`, the `im`/`am` text objects and `ScrollMarkUp`/`ScrollMarkDown`
visit user marks and prompt marks alike, but a user mark never starts a command block, so it neither
splits a fold nor decides where old output is dropped from the scrollback.

```

- [ ] **Step 9: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test vthost_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[vi]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[folding]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[history-eviction]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[promptregion]"`, `out/build/clangcl-debug/bin/vthost_test.exe`
Expected: zero warnings (the `GridWire.cpp` count assertion compiles); `All tests passed` for each.

- [ ] **Step 10: Format and commit**

Run: `clang-format -i src/vtbackend/core/LineFlags.hpp src/vthost/GridWire.cpp src/vtbackend/input/vi/ViCommands.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/shell/PromptRegion.cpp src/vtbackend/grid/Grid.hpp src/vtbackend/input/vi/ViCommands_test.cpp src/vtbackend/screen/Screen_test.cpp src/vtbackend/grid/Grid_test.cpp src/vtbackend/shell/Folding_test.cpp src/vtbackend/grid/Line_test.cpp`

```bash
git add src/vtbackend/core/LineFlags.hpp src/vthost/GridWire.cpp src/vtbackend/input/vi/ViCommands.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/shell/PromptRegion.cpp src/vtbackend/grid/Grid.hpp docs/demo/line-marks.md src/vtbackend/input/vi/ViCommands_test.cpp src/vtbackend/screen/Screen_test.cpp src/vtbackend/grid/Grid_test.cpp src/vtbackend/shell/Folding_test.cpp src/vtbackend/grid/Line_test.cpp
git commit -F - <<'EOF'
vtbackend: make vi `mm` a user mark rather than a prompt mark

LineFlag::UserMark (bit 9, head-only). [m/]m, im/am and ScrollMarkUp/Down
visit both marks through NavigationMarkFlags; block-atomic eviction and
folding keep reading LineFlag::Marked alone, so a bookmark no longer splits
a fold or decides where history is cut.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.7: Inject the terminal's clocks and stamp row birth times

**Files:**
- Create: `src/vtbackend/screen/TerminalClocks.hpp`
- Modify: `src/vtbackend/screen/Terminal.hpp` (includes L19-25; ctor doc and declaration L459-470; `lineBirthTime` after `livePromptSpan()` ~L1739; private `sampleBatchClock` after `private:` L2287; members after `_currentTime` L2424-2425)
- Modify: `src/vtbackend/screen/Terminal.cpp` (ctor L168-183; `processInputOnce` L422; `writeToScreen` L1704; new definitions after `writeToScreen`)
- Modify: `src/vtbackend/screen/Screen.hpp` (`updateCursorIterator` L827-838; `setLineBirthStamp` after `setActiveContextId` L864; member after `_activeContextId` L1239)
- Modify: `src/vtbackend/CMakeLists.txt` (header, test file, `core::platform` link)
- Modify: `src/vtbackend/screen/Screen_test.cpp` (`Delta.imagePlacementBumpsExactlyTheCoveredLines`, ~L4413)
- Modify every `Terminal` construction site (12):
  `src/vtbackend/testing/MockTerm.hpp:185,289`, `src/vtbackend/screen/Terminal_test.cpp:3001,3035`, `src/vtbackend/bench-headless.cpp:440`, `src/vtconformance/TerminalEngine.cpp:49`, `src/vthost/SessionHost.cpp:41`, `src/vthost/tmux/TmuxClientModel.cpp:40`, `src/vthost/client/ScreenMirror_test.cpp:112,244`, `src/contour/session/TerminalSession.cpp:247`, `src/contour/remote/NativeController_test.cpp:315,1336`
- Test: create `src/vtbackend/screen/Terminal_blocks_test.cpp`

**Interfaces:**
- Consumes: `Line::bornAt/stampBornAt/MaxBornAt` (Task 1.4); `core::platform::{IClock, WallClockRef, ManualClock, ManualWallClock, defaultSteadyClock, defaultSystemWallClock}`.
- Produces (C1): `struct TerminalClocks { core::platform::IClock const& steady; core::platform::WallClockRef wall; [[nodiscard]] static TerminalClocks system() noexcept; };`; `Terminal(Events&, core::Environment const&, std::unique_ptr<vtpty::Pty>, Settings, TerminalClocks, std::chrono::steady_clock::time_point)`; `[[nodiscard]] std::optional<std::chrono::system_clock::time_point> Terminal::lineBirthTime(Line const&) const noexcept`; `void Screen::setLineBirthStamp(uint32_t secondsPlusOne) noexcept`. `MockTerm` gains `core::platform::ManualClock steadyClock; core::platform::ManualWallClock wallClock;`.

- [ ] **Step 1: Write the failing tests** — create `src/vtbackend/screen/Terminal_blocks_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/Utils.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace
{

/// A wall clock that counts its reads, so a test can pin "once per PTY batch" rather than infer it.
class CountingWallClock final: public core::platform::IWallClock
{
  public:
    [[nodiscard]] std::chrono::system_clock::time_point now() const noexcept override
    {
        ++_reads;
        return std::chrono::system_clock::time_point {};
    }

    [[nodiscard]] int reads() const noexcept { return _reads; }

  private:
    mutable int _reads = 0;
};

/// The birth stamp of the primary screen's row @p line.
[[nodiscard]] uint32_t bornAtRow(MockTerm<>& mc, int line)
{
    return mc.terminal.primaryScreen().grid().lineAt(LineOffset(line)).bornAt();
}

/// What MockTerm's wall clock reads when the terminal is built: the epoch row stamps count from.
[[nodiscard]] std::chrono::system_clock::time_point sessionEpoch()
{
    return std::chrono::system_clock::time_point {};
}

} // namespace

// {{{ row birth times

TEST_CASE("Terminal.blocks.aRowIsBornWhenTheCursorFirstReachesIt", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    mc.wallClock.advance(5s);
    mc.writeToScreen("one\r\n"); // rows 0 and 1 are reached in this batch
    mc.wallClock.advance(10s);
    mc.writeToScreen("two\r\nthree"); // row 1 was reached already; row 2 is new

    CHECK(bornAtRow(mc, 0) == 6); // seconds since the session started, plus one
    CHECK(bornAtRow(mc, 1) == 6);
    CHECK(bornAtRow(mc, 2) == 16);
    CHECK(bornAtRow(mc, 3) == 0); // never reached

    auto const& grid = mc.terminal.primaryScreen().grid();
    CHECK(mc.terminal.lineBirthTime(grid.lineAt(LineOffset(2))) == sessionEpoch() + 15s);
    CHECK_FALSE(mc.terminal.lineBirthTime(grid.lineAt(LineOffset(3))).has_value());
}

TEST_CASE("Terminal.blocks.returningToARowKeepsItsFirstTime", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.advance(5s);
    mc.writeToScreen("a\r\nb");
    mc.wallClock.advance(60s);
    mc.writeToScreen("\033[Hx"); // back to row 0, and write over it

    CHECK(bornAtRow(mc, 0) == 6);
}

TEST_CASE("Terminal.blocks.aRecycledRowIsBornAgain", "[terminal][semanticblocks]")
{
    // With no scrollback, the row that scrolls off the top is reset and reused at the bottom: a new line,
    // whose time is the time the cursor reached it in its new life.
    auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(10) }, LineCount(0) };
    mc.wallClock.advance(5s);
    mc.writeToScreen("a\r\nb");
    mc.wallClock.advance(20s);
    mc.writeToScreen("\r\nc");

    CHECK(bornAtRow(mc, 0) == 6);  // "b", scrolled up from row 1
    CHECK(bornAtRow(mc, 1) == 26); // "c", on the recycled row
}

TEST_CASE("Terminal.blocks.theWallClockIsReadOncePerBatch", "[terminal][semanticblocks]")
{
    // Spec §5.4: no clock read per line. A batch that crosses five rows reads the wall clock once.
    auto events = Terminal::NullEvents {};
    auto steady = core::platform::ManualClock {};
    auto wall = CountingWallClock {};
    auto const pageSize = PageSize { LineCount(10), ColumnCount(20) };
    auto settings = Settings {};
    settings.pageSize = pageSize;
    auto terminal = Terminal { events,
                               core::defaultEnvironment(),
                               std::make_unique<vtpty::MockPty>(pageSize),
                               std::move(settings),
                               TerminalClocks { .steady = steady, .wall = wall },
                               std::chrono::steady_clock::time_point {} };
    auto const atBirth = wall.reads();

    terminal.writeToScreen("1\r\n2\r\n3\r\n4\r\n5\r\n");
    CHECK(wall.reads() - atBirth == 1);

    terminal.writeToScreen("6\r\n");
    CHECK(wall.reads() - atBirth == 2);
}

TEST_CASE("Terminal.blocks.alternateScreenRowsAreNeverStamped", "[terminal][semanticblocks]")
{
    // The gutter shows nothing on the alternate screen, so its rows carry no time -- and a full-screen
    // application repainting every row must not dirty them for a stamp nobody reads.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.advance(5s);
    mc.writeToScreen("\033[?1049h");
    mc.writeToScreen("x\r\ny\r\nz");

    auto const& alternate = mc.terminal.alternateScreen().grid();
    for (auto const line: std::views::iota(0, 3))
        CHECK(alternate.lineAt(LineOffset(line)).bornAt() == 0);
}

TEST_CASE("Terminal.blocks.aStampPastTheHorizonSaturatesAndShowsNoTime", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.advance(std::chrono::seconds { Line::MaxBornAt } + 10s);
    mc.writeToScreen("x");

    auto const& line = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    CHECK(line.bornAt() == Line::MaxBornAt);
    CHECK_FALSE(mc.terminal.lineBirthTime(line).has_value());
}

TEST_CASE("Terminal.blocks.aWallClockStepBackwardsStampsTheSessionStart", "[terminal][semanticblocks]")
{
    // NTP may step the wall clock back. A row cannot be born before its session was.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.setNow(sessionEpoch() - 1h);
    mc.writeToScreen("x");

    auto const& line = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    CHECK(line.bornAt() == 1);
    CHECK(mc.terminal.lineBirthTime(line) == sessionEpoch());
}

// }}}
```

and register it in `src/vtbackend/CMakeLists.txt`'s `vtbackend_test` list, after `screen/Terminal_input_test.cpp`:

```cmake
        screen/Terminal_input_test.cpp
        screen/Terminal_blocks_test.cpp
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'wallClock': is not a member of 'vtbackend::MockTerm<vtpty::MockPty>'` and `'TerminalClocks': undeclared identifier`.

- [ ] **Step 3: Create `src/vtbackend/screen/TerminalClocks.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

namespace vtbackend
{

/// The clocks a Terminal reads; injected so tests drive time deterministically.
///
/// Two, because they answer different questions: the steady clock measures how long a command ran (an NTP
/// step must not make a build take minus three seconds), the wall clock says when something happened in
/// the user's terms (a row's birth time, a block's start). Both are BORROWED: whoever builds the Terminal
/// keeps them alive for its whole life.
struct TerminalClocks
{
    /// Monotonic time, for durations.
    core::platform::IClock const& steady;

    /// Calendar time, for the times a user reads.
    core::platform::WallClockRef wall;

    /// The production clocks (process-wide defaults from core::platform).
    /// @return Clocks reading std::chrono::steady_clock and std::chrono::system_clock.
    [[nodiscard]] static TerminalClocks system() noexcept
    {
        return TerminalClocks { .steady = core::platform::defaultSteadyClock(),
                                .wall = core::platform::defaultSystemWallClock() };
    }
};

} // namespace vtbackend
```

and register it in `src/vtbackend/CMakeLists.txt`: in `vtbackend_HEADERS` after `screen/Terminal.hpp` add `screen/TerminalClocks.hpp`; in `target_link_libraries(vtbackend PUBLIC …)` after `core::log` add `core::platform` (Terminal.hpp now names its clock types, so the dependency is stated rather than inherited through vtpty).

- [ ] **Step 4: Change the `Terminal` constructor** — in `src/vtbackend/screen/Terminal.hpp` add `#include <vtbackend/screen/TerminalClocks.hpp>` after `#include <vtbackend/screen/StatusLineBuilder.hpp>`, and replace

```cpp
    /// @param factorySettings The settings a hard reset (RIS) restores.
    /// @param now             The current time, as the caller's clock reads it.
    Terminal(Events& eventListener,
             core::Environment const& env,
             std::unique_ptr<vtpty::Pty> pty,
             Settings factorySettings,
             std::chrono::steady_clock::time_point now /* = std::chrono::steady_clock::now()*/);
```

with

```cpp
    /// @param factorySettings The settings a hard reset (RIS) restores.
    /// @param clocks          The clocks command durations and row birth times are read from. Borrowed:
    ///                        the caller keeps them alive for the terminal's whole life. Production
    ///                        passes TerminalClocks::system().
    /// @param now             The current time, as the caller's clock reads it.
    Terminal(Events& eventListener,
             core::Environment const& env,
             std::unique_ptr<vtpty::Pty> pty,
             Settings factorySettings,
             TerminalClocks clocks,
             std::chrono::steady_clock::time_point now /* = std::chrono::steady_clock::now()*/);
```

Replace the member

```cpp
    // terminal clock
    std::chrono::steady_clock::time_point _currentTime;
```

with

```cpp
    // terminal clock
    std::chrono::steady_clock::time_point _currentTime;

    /// The clocks this terminal reads (@see TerminalClocks) -- never std::chrono::*::now() directly.
    TerminalClocks _clocks;

    /// What the wall clock read when this terminal was built. Line::bornAt() counts seconds from here,
    /// which is what lets a row's time fit the three bytes of padding it has.
    std::chrono::system_clock::time_point _sessionEpoch;
```

In `src/vtbackend/screen/Terminal.cpp` replace

```cpp
                   Settings factorySettings,
                   chrono::steady_clock::time_point now):
```

with

```cpp
                   Settings factorySettings,
                   TerminalClocks clocks,
                   chrono::steady_clock::time_point now):
```

and

```cpp
    _currentTime { now },
    _ptyBufferPool { core::nextPowerOfTwo(_settings.ptyBufferObjectSize) },
```

with

```cpp
    _currentTime { now },
    _clocks { clocks },
    _sessionEpoch { clocks.wall.now() },
    _ptyBufferPool { core::nextPowerOfTwo(_settings.ptyBufferObjectSize) },
```

- [ ] **Step 5: Stamp rows as the cursor reaches them** — in `src/vtbackend/screen/Screen.hpp` replace

```cpp
    /// Re-points _currentLine at the row the cursor is on, and stamps that row with the context in
    /// effect.
```

with

```cpp
    /// Re-points _currentLine at the row the cursor is on, and stamps that row with the context in
    /// effect and, if it carries none yet, the time the cursor first reached it (@see setLineBirthStamp).
```

and

```cpp
        _currentLine = &_grid.lineAt(_cursor.position.line);
        _currentLine->adoptContext(_activeContextId);
    }
```

with

```cpp
        _currentLine = &_grid.lineAt(_cursor.position.line);
        _currentLine->adoptContext(_activeContextId);
        _currentLine->stampBornAt(_lineBirthStamp);
    }
```

After the closing `}` of `setActiveContextId(ContextId)` insert:

```cpp
    /// The birth stamp (@see Line::bornAt) rows the cursor reaches from now on are given.
    ///
    /// Set by Terminal once per PTY batch, on the primary screen only: the alternate screen and the status
    /// lines never show a time, so their rows stay unstamped and are never dirtied for one. The line the
    /// cursor is ALREADY on is stamped too when it has no stamp yet -- the cursor sat on it before any
    /// batch ran, or ED reset it underneath the cursor -- and either way this is the first batch it is
    /// seen in.
    /// @param secondsPlusOne Seconds since the session began, plus one; zero stamps nothing.
    void setLineBirthStamp(uint32_t secondsPlusOne) noexcept
    {
        _lineBirthStamp = secondsPlusOne;
        if (_currentLine)
            _currentLine->stampBornAt(secondsPlusOne);
    }
```

and after the member `ContextId _activeContextId {};` insert:

```cpp

    /// The birth stamp freshly reached rows get. @see setLineBirthStamp.
    uint32_t _lineBirthStamp = 0;
```

- [ ] **Step 6: Sample the wall clock once per batch** — in `src/vtbackend/screen/Terminal.hpp`, directly after the `  private:` at ~L2287, insert:

```cpp
    /// Reads the wall clock ONCE for the batch about to be parsed and hands the primary screen the stamp
    /// every row the cursor first reaches during it is born with -- spec §5.4: no clock read per line.
    /// Requires _stateMutex to be held.
    void sampleBatchClock() noexcept;
```

and after `livePromptSpan() const;` (public) insert:

```cpp

    /// When the cursor first reached @p line, by the wall clock this terminal was given.
    ///
    /// Read from the 24-bit stamp the line carries (@see Line::bornAt): nullopt for a line the cursor
    /// never reached, a line of a screen that is not stamped (the alternate screen), and a line stamped
    /// past the ~194-day horizon, whose saturated stamp no longer says when.
    /// @param line A line of this terminal's primary screen.
    /// @return The instant, to the second.
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> lineBirthTime(Line const& line) const noexcept;
```

In `src/vtbackend/screen/Terminal.cpp`, in `processInputOnce()` replace

```cpp
        auto const _ = std::lock_guard { *this };
        // Use the buffer that readFromPty() actually read into, not _currentPtyBuffer
```

with

```cpp
        auto const _ = std::lock_guard { *this };
        sampleBatchClock();
        // Use the buffer that readFromPty() actually read into, not _currentPtyBuffer
```

in `writeToScreen(string_view)` replace

```cpp
        auto const l = std::lock_guard { *this };
        parseFragmentChunked(vtStream);
```

with

```cpp
        auto const l = std::lock_guard { *this };
        sampleBatchClock();
        parseFragmentChunked(vtStream);
```

and after the closing `}` of `Terminal::writeToScreen` add:

```cpp
void Terminal::sampleBatchClock() noexcept
{
    // Floored, then clamped below at the session's first second: a wall clock stepped back before the
    // session began must not wrap a row into the far future.
    auto const elapsed = std::chrono::floor<std::chrono::seconds>(_clocks.wall.now() - _sessionEpoch).count();
    auto const stamp = std::clamp<int64_t>(static_cast<int64_t>(elapsed) + 1, 1, static_cast<int64_t>(Line::MaxBornAt));
    primaryScreen().setLineBirthStamp(static_cast<uint32_t>(stamp));
}

std::optional<std::chrono::system_clock::time_point> Terminal::lineBirthTime(Line const& line) const noexcept
{
    auto const stamp = line.bornAt();
    if (stamp == 0 || stamp >= Line::MaxBornAt)
        return std::nullopt;
    return _sessionEpoch + std::chrono::seconds { stamp - 1 };
}
```

- [ ] **Step 7: Give `MockTerm` manual clocks** — in `src/vtbackend/testing/MockTerm.hpp` add `#include <core/platform/Clock.hpp>` after `#include <core/Environment.hpp>`; replace

```cpp
    Terminal terminal;
```

with

```cpp
    /// The clocks the terminal reads, driven by the test: steadyClock for command durations, wallClock for
    /// row birth times and block times. Both start at their epoch, which is also the session epoch row
    /// stamps count from. Declared BEFORE `terminal`, which borrows them for its whole life.
    core::platform::ManualClock steadyClock;
    core::platform::ManualWallClock wallClock;

    Terminal terminal;
```

and in the out-of-line constructor replace

```cpp
               createSettings(pageSize, historyLimits, ptyReadBufferSize),
               std::chrono::steady_clock::time_point() } // explicitly start with empty timepoint
```

with

```cpp
               createSettings(pageSize, historyLimits, ptyReadBufferSize),
               TerminalClocks { .steady = steadyClock, .wall = wallClock },
               std::chrono::steady_clock::time_point() } // explicitly start with empty timepoint
```

- [ ] **Step 8: Update the remaining construction sites** — each gains one argument before the time point. Production code and tests that do not care about time pass `vtbackend::TerminalClocks::system()`:

`src/vtbackend/screen/Terminal_test.cpp` ~L3001 and ~L3035 — after the line `settings,` insert `vtbackend::TerminalClocks::system(),` (both sites).

`src/vtbackend/bench-headless.cpp` ~L440:

```cpp
    auto terminal = vtbackend::Terminal {
        events, env, std::move(ownedProcess), settings, vtbackend::TerminalClocks::system(), std::chrono::steady_clock::now()
    };
```

`src/vtconformance/TerminalEngine.cpp` ~L49:

```cpp
    _terminal = std::make_unique<vtbackend::Terminal>(*this,
                                                      core::defaultEnvironment(),
                                                      std::move(device),
                                                      settings,
                                                      vtbackend::TerminalClocks::system(),
                                                      std::chrono::steady_clock::now());
```

`src/vthost/SessionHost.cpp` ~L41:

```cpp
    _terminal(_events,
              env,
              std::move(pty),
              std::move(settings),
              vtbackend::TerminalClocks::system(),
              std::chrono::steady_clock::now()),
```

`src/vthost/tmux/TmuxClientModel.cpp` ~L40, `src/vthost/client/ScreenMirror_test.cpp` ~L112 and ~L244, `src/contour/remote/NativeController_test.cpp` ~L315 and ~L1336 — after the line `std::move(settings),` insert `vtbackend::TerminalClocks::system(),` (five sites).

`src/contour/session/TerminalSession.cpp` ~L247:

```cpp
    _terminal { *this,
                app.processEnvironment(),
                std::move(pty),
                config::sessionSettings(_config, _profile, _currentColorPreference, initialPageSize),
                vtbackend::TerminalClocks::system(),
                std::chrono::steady_clock::now() },
```

A missed site is a compile error (the five-argument constructor no longer exists), so building every target that links `vtbackend` in Step 9 is the completeness check; `bench-headless` is built only with `-DLIBTERMINAL_BUILD_BENCH_HEADLESS=ON`, so review its hunk by eye.

`src/vtbackend/screen/Screen_test.cpp` ~L4413, in `TEST_CASE("Delta.imagePlacementBumpsExactlyTheCoveredLines", "[screen][delta]")` — the sixel's trailing `linefeed()` now carries the cursor onto row 10 for the first time, which stamps and dirties it; visit row 10 before draining so the test keeps pinning exactly the rows the image covers. Replace

```cpp
    auto& grid = mock.terminal.primaryScreen().grid();
    auto cursor = drainedDeltaCursor(grid);
```

with

```cpp
    auto& grid = mock.terminal.primaryScreen().grid();
    mock.writeToScreen("\033[11;1H\033[H"); // a row's first visit stamps its birth time (and dirties it once): visit row 10 before draining
    auto cursor = drainedDeltaCursor(grid);
```

- [ ] **Step 9: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test vtrasterizer_test vtworkspace_test vthost_test vtconformance_test contour_test contour_gui_test`
then `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[delta]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[context]"`, `out/build/clangcl-debug/bin/vthost_test.exe`, `out/build/clangcl-debug/bin/vtconformance_test.exe`, `out/build/clangcl-debug/bin/contour_gui_test.exe "[attach]"`
Expected: zero warnings; `All tests passed` for each. (`[delta]` and `vthost_test` prove the once-per-line dirtying of a first stamp disturbs no delta count.) If `contour_gui_test` fails to link on this machine's pre-existing `src/contour/display/*` `yaml-cpp/emitter.h` break, compile `TerminalSession.cpp` and `NativeController_test.cpp` alone: `ninja -C out/build/clangcl-debug -t targets all | grep -E "TerminalSession.cpp|NativeController_test.cpp"` to get the object names, then `ninja -C out/build/clangcl-debug <object>` for each (memory: windows-verify-tricks).

- [ ] **Step 10: Format and commit**

Run: `clang-format -i src/vtbackend/screen/TerminalClocks.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/testing/MockTerm.hpp src/vtbackend/screen/Terminal_test.cpp src/vtbackend/screen/Terminal_blocks_test.cpp src/vtbackend/screen/Screen_test.cpp src/vtbackend/bench-headless.cpp src/vtconformance/TerminalEngine.cpp src/vthost/SessionHost.cpp src/vthost/tmux/TmuxClientModel.cpp src/vthost/client/ScreenMirror_test.cpp src/contour/session/TerminalSession.cpp src/contour/remote/NativeController_test.cpp`

```bash
git add src/vtbackend/screen/TerminalClocks.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/testing/MockTerm.hpp src/vtbackend/screen/Terminal_test.cpp src/vtbackend/screen/Terminal_blocks_test.cpp src/vtbackend/screen/Screen_test.cpp src/vtbackend/CMakeLists.txt src/vtbackend/bench-headless.cpp src/vtconformance/TerminalEngine.cpp src/vthost/SessionHost.cpp src/vthost/tmux/TmuxClientModel.cpp src/vthost/client/ScreenMirror_test.cpp src/contour/session/TerminalSession.cpp src/contour/remote/NativeController_test.cpp
git commit -F - <<'EOF'
vtbackend: inject the terminal's clocks and stamp row birth times

Terminal takes TerminalClocks (steady + wall) at construction. The wall
clock is read once per PTY batch, and each primary-screen row is stamped
with the second the cursor first reached it; lineBirthTime() turns the
stamp back into a time. MockTerm drives both clocks by hand.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.8: The terminal owns the store; OSC 133 feeds it

**Files:**
- Modify: `src/vtbackend/screen/Settings.hpp` (include; field after `foldJumpBehavior` ~L173)
- Modify: `src/vtbackend/screen/Terminal.hpp` (include; `Events` after `contextChanged` ~L343; `NullEvents` after its `contextChanged` override ~L437; public block-store API after `lineBirthTime`; member after `_semanticBlockTracker` ~L2862)
- Modify: `src/vtbackend/screen/Terminal.cpp` (include `core/WorkingDirectory.hpp`; `hardReset` before `// Reset all pages.` ~L4427; new section after `setContextChain` ~L3815)
- Modify: `src/vtbackend/screen/Screen.hpp` (`updateCursorIterator`; `setActiveBlockId` after `setLineBirthStamp`; member; private `recoverCommandLine`/`isPrimaryPage` after `processShellIntegration` ~L1222)
- Modify: `src/vtbackend/screen/Screen.cpp` (`processShellIntegration` cases `A`, `C`, `D` ~L6781-6843; new definitions after `processShellIntegration`)
- Modify: `src/vtbackend/testing/MockTerm.hpp` (identity + finish recorder after `searchPromptRequested` ~L251)
- Modify: `src/contour/session/TerminalSession.hpp:564-568` (`override`)
- Test: `src/vtbackend/screen/Terminal_blocks_test.cpp` (append)

**Interfaces:**
- Consumes: Tasks 1.2-1.7; `resolveWorkingDirectory`, `CwdPurpose::Display` (`core/WorkingDirectory.hpp`); `Screen::livePromptSpan()`; `Grid::logicalLineHead`, `stableLineIdOf`, `stableIdGeneration`, `changingLineAt`, `addressableTop`.
- Produces (C1): `Settings::commandBlockLimits`; `virtual void Terminal::Events::commandBlockFinished(CommandBlockSummary const&)`; `[[nodiscard]] virtual LocalIdentity Terminal::Events::localIdentity() const noexcept` (addition); `[[nodiscard]] CommandBlockStore& Terminal::commandBlocks() noexcept` and its `const` overload; `[[nodiscard]] CommandBlockRecord const* Terminal::commandBlockAt(LineOffset) const noexcept`; `void Screen::setActiveBlockId(CommandBlockId) noexcept`; additions `Terminal::beginCommandBlock()`, `beginCommand(std::optional<std::string>, CommandLineSource)`, `endCommand(int, ContextOutcome)`, `[[nodiscard]] WorkingDirectorySnapshot workingDirectorySnapshot() const`. `MockTerm` gains `localMachineId`, `localHostName`, `finishedCommandBlocks`.

- [ ] **Step 1: Write the failing tests** — append to `src/vtbackend/screen/Terminal_blocks_test.cpp`:

```cpp
// {{{ OSC 133 feeds the store

namespace
{

/// The id of the block the primary screen's row @p line belongs to.
[[nodiscard]] CommandBlockId blockOfRow(MockTerm<>& mc, int line)
{
    return mc.terminal.primaryScreen().grid().lineAt(LineOffset(line)).blockId();
}

/// A prompt with its ;B, the command the user typed, and the Enter that runs it -- everything up to, not
/// including, the ;C.
void typeAtPrompt(MockTerm<>& mc, std::string_view typed)
{
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ");
    mc.writeToScreen("\033]133;B\033\\");
    mc.writeToScreen(typed);
    mc.writeToScreen("\r\n");
}

} // namespace

TEST_CASE("Terminal.blocks.aPromptStartMintsARecordAndStampsItsRow", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("\033]133;A\033\\$ ");

    REQUIRE(mc.terminal.commandBlocks().size() == 1);
    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->state == CommandBlockState::Prompting);
    CHECK(blockOfRow(mc, 0) == record->id);
    CHECK(mc.terminal.commandBlockAt(LineOffset(0)) == record);
    CHECK(record->headStableId == mc.terminal.primaryScreen().grid().stableLineIdOf(LineOffset(0)));
}

TEST_CASE("Terminal.blocks.everyRowOfABlockCarriesItsIdAndTheNextPromptStartsANewOne", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) } };
    typeAtPrompt(mc, "ls");
    mc.writeToScreen("\033]133;C\033\\a\r\nb\r\n\033]133;D;0\033\\");
    REQUIRE(mc.terminal.commandBlocks().lastFinished() != nullptr);
    auto const first = mc.terminal.commandBlocks().lastFinished()->id;
    mc.writeToScreen("\033]133;A\033\\$ ");
    auto const second = mc.terminal.commandBlocks().currentId();

    CHECK(blockOfRow(mc, 0) == first); // "$ ls"
    CHECK(blockOfRow(mc, 1) == first); // "a"
    CHECK(blockOfRow(mc, 2) == first); // "b"
    // The ;D and the next ;A land on the same row: last writer wins, as for OSC 3008 contexts.
    CHECK(blockOfRow(mc, 3) == second);
    CHECK(second != first);
}

TEST_CASE("Terminal.blocks.aPromptOnAWrappedOutputLineClaimsTheWholeLogicalLine", "[terminal][semanticblocks]")
{
    // Output wider than the page with no trailing newline: precmd's ;D and ;A land on the LAST physical
    // row of it. ;A marks the logical line's head, and reflow hands every chunk the head's id -- so the new
    // block owns the whole logical line, or the next resize would hand its prompt back to the previous one.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(10) } };
    typeAtPrompt(mc, "wide");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen(std::string(25, 'x')); // rows 1..3
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

    auto const second = mc.terminal.commandBlocks().currentId();
    CHECK(blockOfRow(mc, 1) == second);
    CHECK(blockOfRow(mc, 2) == second);
    CHECK(blockOfRow(mc, 3) == second);
    REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
    CHECK(mc.terminal.commandBlocks().current()->headStableId
          == mc.terminal.primaryScreen().grid().stableLineIdOf(LineOffset(1)));
}

TEST_CASE("Terminal.blocks.theCommandLineComesFromTheShellWhenItSaysSo", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    typeAtPrompt(mc, "ls -la");
    mc.writeToScreen("\033]133;C;cmdline_url=ls%20-la%20%2Ftmp\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine == "ls -la /tmp"); // what the shell said, not what the grid shows
    CHECK(record->commandLineSource == CommandLineSource::Reported);
}

TEST_CASE("Terminal.blocks.theCommandLineIsRecoveredFromTheGridAfterPromptEnd", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) } };

    SECTION("a one-line command")
    {
        typeAtPrompt(mc, "ls -la");
        mc.writeToScreen("\033]133;C\033\\");
        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        CHECK(record->commandLine == "ls -la");
        CHECK(record->commandLineSource == CommandLineSource::Recovered);
    }

    SECTION("a command that wrapped is read back whole")
    {
        typeAtPrompt(mc, "0123456789abc"); // "$ " + 13 columns on a 10-column page
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine == "0123456789abc");
    }

    SECTION("a continued command keeps its line break")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\echo a \\\r\n> b\r\n");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine == "echo a \\\n> b");
    }
}

TEST_CASE("Terminal.blocks.withoutPromptEndNothingIsRecovered", "[terminal][semanticblocks]")
{
    // Without ;B the terminal cannot tell the prompt's own text from what was typed into it, and a command
    // line that might read "user@host:~$ ls" is worse than none.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    mc.writeToScreen("\033]133;A\033\\$ ls\r\n\033]133;C\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLineSource == CommandLineSource::None);
    CHECK(record->commandLine.empty());
}

TEST_CASE("Terminal.blocks.aHugeCmdlineUrlIsStoredTruncatedAtACodepoint", "[terminal][semanticblocks]")
{
    // Review Focus #3 end to end: 64 KiB of percent-encoded "€" through the real parser (which itself caps
    // an OSC at Sequence::MaxOscLength) into the store.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    auto encoded = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, (64 * 1024 / 9) + 1))
        encoded += "%E2%82%AC";
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("\033]133;C;cmdline_url=" + encoded + "\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    auto const& line = record->commandLine;
    CHECK(line.size() <= MaxRecordedCommandLineBytes);
    REQUIRE(line.ends_with("\xE2\x80\xA6"));
    auto const kept = std::string_view { line }.substr(0, line.size() - 3);
    CHECK(kept.size() % 3 == 0);
    CHECK(std::ranges::all_of(std::views::iota(size_t { 0 }, kept.size() / 3),
                              [&](size_t i) { return kept.substr(i * 3, 3) == "\xE2\x82\xAC"; }));
}

TEST_CASE("Terminal.blocks.aReportedFinishIsAnnouncedOnce", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    typeAtPrompt(mc, "make");
    mc.writeToScreen("\033]133;C\033\\");
    mc.steadyClock.advance(2s);
    mc.writeToScreen("done\r\n\033]133;D;3\033\\");

    REQUIRE(mc.finishedCommandBlocks.size() == 1);
    auto const& summary = mc.finishedCommandBlocks.front();
    CHECK(summary.commandLine == "make");
    CHECK(summary.exitCode == 3);
    CHECK(summary.duration == 2s);

    // A second ;D with nothing running (tcsh) announces nothing more.
    mc.writeToScreen("\033]133;D;0\033\\");
    CHECK(mc.finishedCommandBlocks.size() == 1);
}

TEST_CASE("Terminal.blocks.aNestedPromptClosesTheRunningBlockWithoutAnnouncingIt", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    typeAtPrompt(mc, "ssh host");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen("\033]133;A\033\\remote$ "); // the remote shell's own prompt

    CHECK(mc.finishedCommandBlocks.empty());
    auto const* outer = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(outer != nullptr);
    CHECK(outer->end == CommandBlockEnd::Implicit);
    CHECK(outer->commandLine == "ssh host");
}

TEST_CASE("Terminal.blocks.theWorkingDirectoryIsSnapshottedAtTheCommandStart", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };

    SECTION("from OSC 7")
    {
        mc.writeToScreen("\033]7;file:///home/user/src\033\\");
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\");
        // A later OSC 7 (the next precmd) must not rewrite where this command ran.
        mc.writeToScreen("\033]7;file:///tmp\033\\");

        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        CHECK(record->workingDirectory.path == "/home/user/src");
        CHECK(record->workingDirectory.locality == ContextLocality::Local);
    }

    SECTION("from OSC 7 naming another host")
    {
        mc.writeToScreen("\033]7;file://elsewhere/home/user\033\\");
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->workingDirectory.path == "/home/user");
        CHECK(mc.terminal.commandBlocks().current()->workingDirectory.locality == ContextLocality::Foreign);
    }

    SECTION("from an OSC 3008 context this machine's identity vouches for")
    {
        mc.localMachineId = "3deb5353d3ba43d08201c136a47ead7b";
        mc.writeToScreen("\033]3008;start=shell-1;type=shell;machineid=3deb5353d3ba43d08201c136a47ead7b;cwd=/srv\033\\");
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->workingDirectory.path == "/srv");
        CHECK(mc.terminal.commandBlocks().current()->workingDirectory.locality == ContextLocality::Local);
    }
}

TEST_CASE("Terminal.blocks.theAlternateScreenFeedsNoBlock", "[terminal][semanticblocks]")
{
    // Blocks live in the primary screen's history: a head id taken on the alternate grid would name a row
    // of a different ring.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    mc.writeToScreen("\033[?1049h");
    mc.writeToScreen("\033]133;A\033\\$ \033]133;C\033\\x\r\n\033]133;D;0\033\\");

    CHECK(mc.terminal.commandBlocks().size() == 0);
    CHECK(mc.finishedCommandBlocks.empty());
    CHECK(mc.terminal.alternateScreen().grid().lineAt(LineOffset(0)).blockId() == CommandBlockId {});
}

TEST_CASE("Terminal.blocks.aHardResetForgetsEveryBlock", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    typeAtPrompt(mc, "ls");
    mc.writeToScreen("\033]133;C\033\\x\r\n\033]133;D;0\033\\");
    REQUIRE(mc.terminal.commandBlocks().size() == 1);

    mc.writeToScreen("\033c");

    CHECK(mc.terminal.commandBlocks().size() == 0);
    CHECK(mc.terminal.commandBlocks().currentId() == CommandBlockId {});
    CHECK(blockOfRow(mc, 0) == CommandBlockId {});
}

TEST_CASE("Terminal.blocks.aFloodOfPromptsStaysWithinTheCap", "[terminal][semanticblocks]")
{
    // Review Focus #4 end to end: `cat` of a log that contains OSC 133 replays every cycle in it.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) }, LineCount(100) };
    auto const cycle = std::string { "\033]133;A\033\\$ \033]133;C\033\\x\r\n\033]133;D;0\033\\" };
    auto flood = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 2'500))
        flood += cycle;
    mc.writeToScreen(flood);

    CHECK(mc.terminal.commandBlocks().size() == mc.terminal.commandBlocks().limits().maxRecords);
    CHECK(mc.finishedCommandBlocks.size() == 2'500);
}

TEST_CASE("Terminal.blocks.commandBlockAtAnswersNoBlockWhereThereIsNone", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) } };
    mc.writeToScreen("plain output\r\n");

    CHECK(mc.terminal.commandBlockAt(LineOffset(0)) == nullptr);   // no shell integration: no block
    CHECK(mc.terminal.commandBlockAt(LineOffset(99)) == nullptr);  // below the page
    CHECK(mc.terminal.commandBlockAt(LineOffset(-99)) == nullptr); // above the scrollback
}

// }}}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'commandBlocks': is not a member of 'vtbackend::Terminal'`.

- [ ] **Step 3: Configure the limit** — in `src/vtbackend/screen/Settings.hpp` add `#include <vtbackend/shell/CommandBlock.hpp>` after `#include <vtbackend/shell/Folding.hpp>        // FoldJumpBehavior`, and after the member `FoldJumpBehavior foldJumpBehavior = FoldJumpBehavior::Expand;` insert:

```cpp

    /// How many shell command blocks (OSC 133 / OSC 3008) the session remembers. Read once: the store takes
    /// it at construction, so a changed value applies to sessions opened afterwards. @see CommandBlockStore.
    CommandBlockStoreLimits commandBlockLimits {};
```

- [ ] **Step 4: Declare the events** — in `src/vtbackend/screen/Terminal.hpp` add `#include <vtbackend/shell/CommandBlock.hpp>` after `#include <vtbackend/shell/Folding.hpp>`. In `class Events`, after `virtual void contextChanged(ContextId /*activeContext*/) {}` insert:

```cpp

        /// A shell command block finished with a REPORTED end (OSC 133;D, or an OSC 3008 `end=` while OSC
        /// 3008 owns the marks) -- never for a block a newer prompt closed implicitly, such as the `ssh`
        /// the user is still inside.
        ///
        /// Raised on the parser thread with Terminal::_stateMutex ALREADY HELD, with the consequence
        /// progressChanged() documents: do not read terminal state here; defer to the GUI thread. The
        /// summary is a value copy and safe to carry across threads.
        /// @param summary What a frontend needs to announce the finish.
        virtual void commandBlockFinished(CommandBlockSummary const& /*summary*/) {}

        /// This machine's identity, for deciding whether the directory a block ran in is local.
        ///
        /// Read on the parser thread with Terminal::_stateMutex held, at every OSC 133;C: an
        /// implementation answers from data fixed at construction. The default knows nothing, which makes
        /// only an OSC 7 URL with no host provably local -- the conservative answer.
        /// @return Views that stay valid for the listener's lifetime.
        [[nodiscard]] virtual LocalIdentity localIdentity() const noexcept { return {}; }
```

In `class NullEvents`, after `void contextChanged(ContextId /*activeContext*/) override {}` insert:

```cpp
        void commandBlockFinished(CommandBlockSummary const& /*summary*/) override {}
        [[nodiscard]] LocalIdentity localIdentity() const noexcept override { return {}; }
```

- [ ] **Step 5: Declare the store API** — in `src/vtbackend/screen/Terminal.hpp`, after the `lineBirthTime` declaration (Task 1.7) insert:

```cpp

    // {{{ Command blocks (OSC 133, and OSC 3008 standing in for it)

    /// Every shell command block of the primary screen, oldest first. @see CommandBlockStore.
    [[nodiscard]] CommandBlockStore& commandBlocks() noexcept;

    /// @copydoc commandBlocks()
    [[nodiscard]] CommandBlockStore const& commandBlocks() const noexcept;

    /// The block the primary screen's row @p gridLine belongs to.
    /// @param gridLine A grid row (negative addresses the scrollback).
    /// @return Its record, or nullptr when the row is outside the grid, belongs to no block, or its block's
    ///         record has been evicted from the store.
    [[nodiscard]] CommandBlockRecord const* commandBlockAt(LineOffset gridLine) const noexcept;

    /// OSC 133;A (or its OSC 3008 stand-in) on the primary screen: closes or discards the current block
    /// per the store's rules, mints a new one headed at the cursor's logical line, and stamps that line.
    void beginCommandBlock();

    /// OSC 133;C (or its OSC 3008 stand-in) on the primary screen: the current block's command starts.
    /// @param commandLine The command line, when one is known.
    /// @param source Where it came from; CommandLineSource::None leaves the record without one.
    void beginCommand(std::optional<std::string> commandLine, CommandLineSource source);

    /// OSC 133;D (or an OSC 3008 `end=` while OSC 3008 owns the marks): the running command finished.
    /// Raises Events::commandBlockFinished when something did finish.
    /// @param exitCode The exit code reported.
    /// @param outcome How it ended, when OSC 3008 said.
    void endCommand(int exitCode, ContextOutcome outcome);

    /// The working directory a command starting now runs in: the OSC 3008 ancestry first, then OSC 7,
    /// with its locality (@see resolveWorkingDirectory, CwdPurpose::Display).
    /// @return The snapshot; empty when nothing reported a directory.
    [[nodiscard]] WorkingDirectorySnapshot workingDirectorySnapshot() const;
    // }}}
```

and after the member `SemanticBlockTracker _semanticBlockTracker;` insert:

```cpp

    /// Every shell command block of the primary screen. Limits and clocks are fixed at construction: a
    /// session configured differently is a different session, so there is no setter. Cleared by RIS.
    CommandBlockStore _commandBlocks { _settings.commandBlockLimits, _clocks.steady, _clocks.wall };
```

- [ ] **Step 6: Implement the API** — in `src/vtbackend/screen/Terminal.cpp` add `#include <vtbackend/core/WorkingDirectory.hpp>` after `#include <vtbackend/core/Primitives.hpp>`, and after the closing `}` of `Terminal::setContextChain` insert:

```cpp

// {{{ Command blocks
CommandBlockStore& Terminal::commandBlocks() noexcept
{
    return _commandBlocks;
}

CommandBlockStore const& Terminal::commandBlocks() const noexcept
{
    return _commandBlocks;
}

CommandBlockRecord const* Terminal::commandBlockAt(LineOffset gridLine) const noexcept
{
    auto const& grid = primaryScreen().grid();
    if (gridLine < grid.addressableTop() || gridLine >= boxed_cast<LineOffset>(grid.pageSize().lines))
        return nullptr;
    auto const id = grid.lineAt(gridLine).blockId();
    return !id ? nullptr : commandBlocks().find(id);
}

void Terminal::beginCommandBlock()
{
    auto& screen = primaryScreen();
    auto const& grid = screen.grid();
    auto const head = grid.logicalLineHead(screen.cursor().position.line);
    auto const id = _commandBlocks.promptStarted(grid.stableLineIdOf(head), grid.stableIdGeneration());
    screen.setActiveBlockId(id);
}

void Terminal::beginCommand(std::optional<std::string> commandLine, CommandLineSource source)
{
    auto& screen = primaryScreen();
    auto const& grid = screen.grid();
    auto const head = grid.logicalLineHead(screen.cursor().position.line);
    _commandBlocks.commandStarted(CommandStart {
        .commandLine = std::move(commandLine),
        .source = source,
        .workingDirectory = workingDirectorySnapshot(),
        .headStableId = grid.stableLineIdOf(head),
        .headIdGeneration = grid.stableIdGeneration(),
    });
    // A ;C with no ;A before it minted a record of its own, whose rows start here.
    screen.setActiveBlockId(_commandBlocks.currentId());
}

void Terminal::endCommand(int exitCode, ContextOutcome outcome)
{
    // Raised from inside the parse, under _stateMutex, exactly as progressChanged() is.
    if (auto const summary = _commandBlocks.commandFinished(exitCode, outcome))
        _eventListener.commandBlockFinished(*summary);
}

WorkingDirectorySnapshot Terminal::workingDirectorySnapshot() const
{
    // Display, not Spawn: the record says where the command RAN, which is the right answer even behind a
    // boundary. The locality travels with it, so a later "open locally" can still refuse.
    auto const resolved = resolveWorkingDirectory(
        _contexts, _currentWorkingDirectory, _eventListener.localIdentity(), CwdPurpose::Display);
    if (!resolved)
        return {};
    return WorkingDirectorySnapshot { .path = resolved->path, .locality = resolved->locality };
}
// }}}
```

In `Terminal::hardReset()`, replace

```cpp
    // Reset all pages.
    for (auto& page: _pages)
        page->hardReset();
```

with

```cpp
    // Before the pages: their reset re-seats each cursor, which stamps the row it lands on with the active
    // block -- and after RIS there is none.
    _commandBlocks.clear();
    primaryScreen().setActiveBlockId(_commandBlocks.currentId());

    // Reset all pages.
    for (auto& page: _pages)
        page->hardReset();
```

- [ ] **Step 7: Stamp the block id in `Screen`** — in `src/vtbackend/screen/Screen.hpp`, in `updateCursorIterator()` replace

```cpp
        _currentLine->adoptContext(_activeContextId);
        _currentLine->stampBornAt(_lineBirthStamp);
```

with

```cpp
        _currentLine->adoptContext(_activeContextId);
        _currentLine->adoptBlock(_activeBlockId);
        _currentLine->stampBornAt(_lineBirthStamp);
```

after `setLineBirthStamp` (Task 1.7) insert:

```cpp

    /// The command block freshly reached rows are stamped with.
    ///
    /// A mirror of the store's current id rather than a read through it, for the reason
    /// setActiveContextId() gives. Like it, re-stamps (and dirties) the line the cursor is ALREADY on --
    /// all of its LOGICAL line: a prompt that starts on the last row of a wrapped output line marks that
    /// logical line's head, and reflow hands every chunk the head's id, so stamping the cursor's row alone
    /// would hand the prompt back to the previous block on the next resize. Last writer wins. Zero is
    /// ignored for the stamp, as Line::adoptBlock() ignores it.
    /// @param id The block now being written.
    void setActiveBlockId(CommandBlockId id) noexcept;
```

after the member `uint32_t _lineBirthStamp = 0;` insert:

```cpp

    /// The command block freshly reached rows are stamped with. @see setActiveBlockId.
    CommandBlockId _activeBlockId {};
```

and after `void processShellIntegration(Sequence const& seq);` insert:

```cpp

    /// The command line the user typed at the live prompt, read off the grid at OSC 133;C.
    ///
    /// Only possible when the shell marked where its prompt ended (;B): the text from that column up to
    /// the cursor is what was typed. Must run BEFORE the ;C's OutputStart is stamped -- the live-prompt
    /// scan reads a line carrying OutputStart as "a command is running" and finds no prompt.
    /// @return The typed text, trailing blanks dropped, or nullopt without ;B or when nothing was typed.
    [[nodiscard]] std::optional<std::string> recoverCommandLine() const;

    /// Whether this screen is the primary page: the only one whose rows belong to command blocks.
    [[nodiscard]] bool isPrimaryPage() const noexcept;
```

- [ ] **Step 8: Implement them and feed the store** — in `src/vtbackend/screen/Screen.cpp`, in `processShellIntegration`, case `'A'`: replace

```cpp
            _terminal->shellIntegration().promptStart(clickEvents);
            _terminal->semanticBlockTracker().promptStart();
            _terminal->autoCollapseOnNewPrompt();
```

with

```cpp
            _terminal->shellIntegration().promptStart(clickEvents);
            _terminal->semanticBlockTracker().promptStart();
            if (isPrimaryPage())
                _terminal->beginCommandBlock();
            _terminal->autoCollapseOnNewPrompt();
```

case `'C'`: replace

```cpp
            markLogicalLineAtCursor(LineFlag::OutputStart);
            std::optional<std::string> commandLine;
            auto const params = seq.intermediateCharacters().substr(1);
            forEachKeyValue(params, [&](std::string_view key, std::string_view value) {
                if (key == "cmdline_url")
                    commandLine = core::unescapeURL(value);
            });
            _terminal->shellIntegration().commandOutputStart(commandLine);
            _terminal->semanticBlockTracker().commandOutputStart(commandLine);
            break;
```

with

```cpp
            auto commandLine = std::optional<std::string> {};
            auto source = CommandLineSource::None;
            auto const params = seq.intermediateCharacters().substr(1);
            forEachKeyValue(params, [&](std::string_view key, std::string_view value) {
                if (key == "cmdline_url")
                {
                    commandLine = core::unescapeURL(value);
                    source = CommandLineSource::Reported;
                }
            });
            // Recovered BEFORE the OutputStart below is stamped. @see recoverCommandLine.
            if (!commandLine && isPrimaryPage())
            {
                commandLine = recoverCommandLine();
                if (commandLine)
                    source = CommandLineSource::Recovered;
            }

            markLogicalLineAtCursor(LineFlag::OutputStart);

            // The legacy observers hear only what the SHELL reported, exactly as before.
            auto const reported = source == CommandLineSource::Reported ? commandLine : std::nullopt;
            _terminal->shellIntegration().commandOutputStart(reported);
            _terminal->semanticBlockTracker().commandOutputStart(reported);
            if (isPrimaryPage())
                _terminal->beginCommand(std::move(commandLine), source);
            break;
```

case `'D'`: replace

```cpp
            _terminal->shellIntegration().commandFinished(exitCode);
            _terminal->semanticBlockTracker().commandFinished(exitCode);
            break;
```

with

```cpp
            _terminal->shellIntegration().commandFinished(exitCode);
            _terminal->semanticBlockTracker().commandFinished(exitCode);
            if (isPrimaryPage())
                _terminal->endCommand(exitCode, ContextOutcome {});
            break;
```

After the closing `}` of `Screen::processShellIntegration` add:

```cpp

void Screen::setActiveBlockId(CommandBlockId id) noexcept
{
    _activeBlockId = id;
    if (!_currentLine || !id)
        return;

    auto const cursorLine = _cursor.position.line;
    auto const head = _grid.logicalLineHead(cursorLine);
    for (auto const row: std::views::iota(unbox<int>(head), unbox<int>(cursorLine) + 1))
    {
        // changingLineAt: a logical line's head may sit in the scrollback. @see Grid::changingLineAt.
        auto& line = _grid.changingLineAt(LineOffset::cast_from(row));
        if (line.blockId() == id)
            continue;
        line.adoptBlock(id);
        // Dirtied for the reason setActiveContextId() gives: a mirror only revisits dirty rows.
        line.markDirty();
    }
}

std::optional<std::string> Screen::recoverCommandLine() const
{
    auto const span = livePromptSpan();
    if (!span || !span->inputBegin)
        return std::nullopt;

    // The ;B column is LOGICAL -- it counts across the wraps of the line ;B landed on -- so it is turned
    // back into a physical cell by the page width.
    auto const width = unbox<int>(_grid.pageSize().columns);
    auto const inputColumn = unbox<int>(*span->inputBegin);
    auto const firstLine = _grid.logicalLineHead(span->lastLine) + LineOffset::cast_from(inputColumn / width);
    auto const firstColumn = ColumnOffset::cast_from(inputColumn % width);
    auto const end = cursor().position;
    if (firstLine > end.line)
        return std::nullopt;

    auto text = std::string {};
    for (auto const row: std::views::iota(unbox<int>(firstLine), unbox<int>(end.line) + 1))
    {
        auto const line = LineOffset::cast_from(row);
        auto const from = line == firstLine ? firstColumn : ColumnOffset(0);
        auto const to = line == end.line ? end.column : boxed_cast<ColumnOffset>(_grid.pageSize().columns);
        if (from < to)
            text += _grid.lineAt(line).toUtf8(from, to);

        // A continuation joins its head seamlessly; any other line break is one the user typed.
        if (line != end.line && !_grid.lineAt(line + 1).isFlagEnabled(LineFlag::Wrapped))
        {
            text.resize(core::trimRight(text).size());
            text += '\n';
        }

        // Enough to decide a truncation: the store keeps no more, and a hostile shell must not make the
        // terminal copy an unbounded region of the grid between a ;B and a ;C.
        if (text.size() > MaxRecordedCommandLineBytes)
            break;
    }

    text.resize(core::trimRight(text).size());
    if (text.empty())
        return std::nullopt;
    return text;
}

bool Screen::isPrimaryPage() const noexcept
{
    return this == &_terminal->primaryScreen();
}
```

- [ ] **Step 9: Record finishes and identity in `MockTerm`** — in `src/vtbackend/testing/MockTerm.hpp`, after `void searchPromptRequested() override { ++searchPromptRequests; }` insert:

```cpp

    /// This machine's identity as the terminal is told it, for the locality of a working directory. Empty
    /// by default: then only an OSC 7 URL with no host is provably local.
    std::string localMachineId;
    std::string localHostName;

    [[nodiscard]] LocalIdentity localIdentity() const noexcept override
    {
        return LocalIdentity { .machineId = localMachineId, .hostname = localHostName };
    }

    /// Every command block the terminal announced as finished, in order.
    /// @see Terminal::Events::commandBlockFinished.
    std::vector<CommandBlockSummary> finishedCommandBlocks;

    void commandBlockFinished(CommandBlockSummary const& summary) override
    {
        finishedCommandBlocks.push_back(summary);
    }
```

- [ ] **Step 10: Let the GUI session answer the identity** — in `src/contour/session/TerminalSession.hpp` replace

```cpp
    /// This machine's identity, for deciding whether a context describes it.
    [[nodiscard]] vtbackend::LocalIdentity localIdentity() const noexcept
```

with

```cpp
    /// This machine's identity, for deciding whether a context describes it.
    ///
    /// Also the vtbackend::Terminal::Events hook the terminal reads at OSC 133;C, on the parser thread:
    /// both strings are fixed at construction, so answering there needs no lock.
    [[nodiscard]] vtbackend::LocalIdentity localIdentity() const noexcept override
```

(clang-tidy hand-audit, `src/contour/**`: the `override` satisfies `modernize-use-override` and clang's `-Winconsistent-missing-override`; nothing else changes in that file.)

- [ ] **Step 11: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test vthost_test contour_gui_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe` (the whole suite: the OSC 133 / 2034 tests still drive the legacy tracker unchanged), `out/build/clangcl-debug/bin/vthost_test.exe`
Expected: zero warnings; `All tests passed`. (For `contour_gui_test`, the Task 1.7 Step 9 fallback applies.)

- [ ] **Step 12: Format and commit**

Run: `clang-format -i src/vtbackend/screen/Settings.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/testing/MockTerm.hpp src/vtbackend/screen/Terminal_blocks_test.cpp src/contour/session/TerminalSession.hpp`

```bash
git add src/vtbackend/screen/Settings.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/testing/MockTerm.hpp src/vtbackend/screen/Terminal_blocks_test.cpp src/contour/session/TerminalSession.hpp
git commit -F - <<'EOF'
vtbackend: feed OSC 133 into the command block store

Terminal owns a CommandBlockStore; ;A mints a block and stamps the cursor's
logical line, ;C records the command line (reported, or recovered from the
grid after ;B) and the working directory with its locality, ;D raises
Events::commandBlockFinished for a reported end. Primary screen only; RIS
clears the store.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.9: OSC 3008 command contexts feed the store; enrichment while OSC 133 owns it

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp` (declare `enrichCommandOutcome` after `endCommand`)
- Modify: `src/vtbackend/screen/Terminal.cpp` (define it after `Terminal::endCommand`)
- Modify: `src/vtbackend/screen/Screen.cpp` (`synthesizeSemanticMarks` ~L6676-6757; a helper before it)
- Test: `src/vtbackend/screen/Screen_context_test.cpp` (insert after `Screen.osc3008 fires no callbacks during its first, undecided cycle`, ~L616)

**Interfaces:**
- Consumes: `MarkArbiter::{owner, contextMayMark, contextMayNotify}`; `ContextCommand::{present, commandLine, outcome}`; `CommandBlockStore::enrichOutcome` (Task 1.3); Task 1.8's Terminal API.
- Produces: `void Terminal::enrichCommandOutcome(ContextOutcome outcome)` (addition). Rules (spec §4.3, last row): while `contextMayNotify()`, a `type=shell` update/return-to begins a block, a `type=command` start begins its command (with `cmdline=` as a Reported command line), its `end=` finishes it with the full `ContextOutcome`; while OSC 133 owns the session, a `type=command` `end=` only enriches the matching block's outcome.

- [ ] **Step 1: Write the failing tests** — in `src/vtbackend/screen/Screen_context_test.cpp`, after the closing `}` of `TEST_CASE("Screen.osc3008 fires no callbacks during its first, undecided cycle", "[context]")` insert:

```cpp
TEST_CASE("Screen.osc3008 drives the command block store once it owns the session", "[context][semanticblocks]")
{
    auto mock = makeTerm();

    systemdCycle(mock, "cmd-1");
    mock.writeToScreen(osc3008("end=cmd-1;exit=success"));
    // The first cycle only DECIDES; the store hears from the second on.
    CHECK(mock.terminal.commandBlocks().size() == 0);

    systemdCycle(mock, "cmd-2");
    mock.writeToScreen(osc3008("end=cmd-2;exit=failure;status=139;signal=SIGSEGV"));

    auto const* block = mock.terminal.commandBlocks().lastFinished();
    REQUIRE(block != nullptr);
    CHECK(block->end == CommandBlockEnd::Reported);
    // 128+11, as every POSIX shell spells a SIGSEGV death...
    CHECK(block->exitCode == 139);
    // ...and, unlike OSC 133;D's bare number, the signal itself.
    CHECK(block->outcome.signal == ContextSignal::Segv);
    CHECK(block->workingDirectory.path == "/home/user");
    REQUIRE(mock.finishedCommandBlocks.size() == 1);
    CHECK(mock.finishedCommandBlocks.front().exitCode == 139);
}

TEST_CASE("Screen.osc3008 feeds the store nothing during its first, undecided cycle", "[context][semanticblocks]")
{
    // The cost of deferring, stated plainly: with no OSC 133 anywhere, the session's FIRST command gets line
    // flags but no record -- cheaper than letting a cmdline-less 3008 command race a good OSC 133;C.
    auto mock = makeTerm();
    systemdCycle(mock, "cmd-1");
    mock.writeToScreen(osc3008("end=cmd-1;exit=success"));

    CHECK(mock.terminal.commandBlocks().size() == 0);
    CHECK(mock.finishedCommandBlocks.empty());
    CHECK(mock.terminal.markArbiter().owner() == MarkOwner::ContextSignalling);
}

TEST_CASE("Screen.osc3008 a command context's cmdline is the block's reported command line", "[context][semanticblocks]")
{
    auto mock = makeTerm();
    systemdCycle(mock, "cmd-1");
    mock.writeToScreen(osc3008("end=cmd-1;exit=success"));

    mock.writeToScreen(osc3008("start=shell-uuid;type=shell;cwd=/home/user"));
    mock.writeToScreen("$ make\r\n");
    mock.writeToScreen(osc3008("start=cmd-2;type=command;cmdline=make"));

    auto const* block = mock.terminal.commandBlocks().current();
    REQUIRE(block != nullptr);
    CHECK(block->state == CommandBlockState::Running);
    CHECK(block->commandLine == "make");
    CHECK(block->commandLineSource == CommandLineSource::Reported);
}

TEST_CASE("Screen.osc3008 enriches an OSC 133 block with how its command died", "[context][semanticblocks]")
{
    // OSC 133 owns the marks, so OSC 3008 may not open or close a block -- but its end= still knows HOW the
    // command ended, which OSC 133;D's bare number cannot say. The two arrive in either order.
    auto const runCrashingCommand = [](MockTerm<>& mock) {
        mock.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\crash\r\n");
        mock.writeToScreen(osc3008("start=cmd-1;type=command"));
        mock.writeToScreen("\033]133;C;cmdline_url=crash\033\\");
        mock.writeToScreen("boom\r\n");
    };

    SECTION("the 3008 end arrives before ;D")
    {
        auto mock = makeTerm();
        runCrashingCommand(mock);
        mock.writeToScreen(osc3008("end=cmd-1;exit=crash;signal=SIGSEGV"));

        REQUIRE(mock.terminal.commandBlocks().current() != nullptr);
        CHECK(mock.terminal.commandBlocks().current()->state == CommandBlockState::Running); // not closed
        CHECK(mock.finishedCommandBlocks.empty());

        mock.writeToScreen("\033]133;D;139\033\\");
        REQUIRE(mock.finishedCommandBlocks.size() == 1);
        CHECK(mock.finishedCommandBlocks.front().outcome.signal == ContextSignal::Segv);
        CHECK(mock.finishedCommandBlocks.front().exitCode == 139);
    }

    SECTION("the 3008 end arrives after ;D")
    {
        auto mock = makeTerm();
        runCrashingCommand(mock);
        mock.writeToScreen("\033]133;D;139\033\\");
        mock.writeToScreen(osc3008("end=cmd-1;exit=crash;signal=SIGSEGV"));

        auto const* block = mock.terminal.commandBlocks().lastFinished();
        REQUIRE(block != nullptr);
        CHECK(block->outcome.exit == ContextExit::Crash);
        CHECK(block->outcome.signal == ContextSignal::Segv);
        CHECK(block->exitCode == 139);
        CHECK(mock.finishedCommandBlocks.size() == 1); // never announced twice
    }
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[context]"`
Expected: FAIL — three of the four new cases: `drives the command block store…` and `…cmdline is the block's reported command line` find no record, and both enrichment sections report `outcome.signal == None`; `…feeds the store nothing during its first, undecided cycle` already passes (it pins behaviour this task must keep).

- [ ] **Step 3: Declare and define the enrichment funnel** — in `src/vtbackend/screen/Terminal.hpp`, after the `endCommand` declaration insert:

```cpp

    /// An OSC 3008 command context ended while OSC 133 owns the marks: records HOW the command ended on
    /// the matching block, without closing or announcing anything. @see CommandBlockStore::enrichOutcome.
    /// @param outcome The ENDFIELDs the `end=` carried.
    void enrichCommandOutcome(ContextOutcome outcome);
```

and in `src/vtbackend/screen/Terminal.cpp`, after the closing `}` of `Terminal::endCommand`:

```cpp

void Terminal::enrichCommandOutcome(ContextOutcome outcome)
{
    _commandBlocks.enrichOutcome(outcome);
}
```

- [ ] **Step 4: Feed the store from `synthesizeSemanticMarks`** — in `src/vtbackend/screen/Screen.cpp`, directly before `void Screen::synthesizeSemanticMarks(` insert:

```cpp
namespace
{
    /// The command line an OSC 3008 `type=command` start carried, when it carried a non-empty `cmdline=`.
    [[nodiscard]] std::optional<std::string> commandLineOf(ContextCommand const& command)
    {
        if (!command.present.contains(ContextField::CommandLine) || command.commandLine.empty())
            return std::nullopt;
        return std::string { command.commandLine };
    }
} // namespace

```

In `synthesizeSemanticMarks`, replace

```cpp
    if (!arbiter.contextMayMark())
        return;
```

with

```cpp
    // OSC 133 owns the session, so OSC 3008 may not open or close a block -- but a command's end= still
    // knows HOW it ended (a signal, a crash), which OSC 133;D's bare number cannot say.
    if (arbiter.owner() == MarkOwner::ShellIntegration && transition.subjectType == ContextType::Command
        && transition.kind == ContextTransitionKind::Ended && isPrimaryPage())
        _terminal->enrichCommandOutcome(command.outcome);

    if (!arbiter.contextMayMark())
        return;
```

replace

```cpp
    auto const notify = arbiter.contextMayNotify();
```

with

```cpp
    auto const notify = arbiter.contextMayNotify();
    auto const feedsBlocks = notify && isPrimaryPage();
```

in the `ContextType::Shell` case replace

```cpp
            setMark();
            if (notify)
            {
                _terminal->shellIntegration().promptStart();
                _terminal->semanticBlockTracker().promptStart();
                _terminal->autoCollapseOnNewPrompt();
            }
            return;
```

with

```cpp
            setMark();
            if (feedsBlocks)
                _terminal->beginCommandBlock();
            if (notify)
            {
                _terminal->shellIntegration().promptStart();
                _terminal->semanticBlockTracker().promptStart();
                _terminal->autoCollapseOnNewPrompt();
            }
            return;
```

in the `ContextType::Command` start branch replace

```cpp
                    _terminal->shellIntegration().commandOutputStart(std::nullopt);
                    _terminal->semanticBlockTracker().commandOutputStart(std::nullopt);
                }
            }
```

with

```cpp
                    _terminal->shellIntegration().commandOutputStart(std::nullopt);
                    _terminal->semanticBlockTracker().commandOutputStart(std::nullopt);
                }
                if (feedsBlocks)
                {
                    auto commandLine = commandLineOf(command);
                    auto const source = commandLine ? CommandLineSource::Reported : CommandLineSource::None;
                    _terminal->beginCommand(std::move(commandLine), source);
                }
            }
```

and in the `Ended` branch replace

```cpp
                    _terminal->shellIntegration().commandFinished(exitCode);
                    _terminal->semanticBlockTracker().commandFinished(exitCode);
                }
            }
            return;
```

with

```cpp
                    _terminal->shellIntegration().commandFinished(exitCode);
                    _terminal->semanticBlockTracker().commandFinished(exitCode);
                }
                if (feedsBlocks)
                    _terminal->endCommand(command.outcome.asShellExitCode(), command.outcome);
            }
            return;
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[context]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"`
Expected: zero warnings; `All tests passed` (the two legacy tracker cases in this file still pass; Task 1.10 retires them).

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Screen_context_test.cpp`

```bash
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Screen_context_test.cpp
git commit -F - <<'EOF'
vtbackend: feed OSC 3008 command contexts into the block store

While OSC 3008 owns the marks its shell/command contexts open, start and
finish blocks, with cmdline= as the reported command line and the full
ContextOutcome. While OSC 133 owns them, a command context's end= only
enriches the matching block with how the command died.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.10: Rebase the mode 2034 reader (`SemanticBlockTracker`) onto the store

**Files:**
- Modify (rewrite): `src/vtbackend/shell/SemanticBlockTracker.hpp`, `src/vtbackend/shell/SemanticBlockTracker.cpp`
- Modify: `src/vtbackend/screen/Screen.hpp` (forward declarations L52-55; query handler declarations L1223-1228)
- Modify: `src/vtbackend/screen/Screen.cpp` (`formatBlockJson` L6250-6262; `handleSemanticBlockQuery` L6421-6464; `handleInProgressQuery` L6466-6473, L6520; `handleCompletedBlocksQuery` L6524-6560; the six `semanticBlockTracker()` feeding calls in `synthesizeSemanticMarks` and `processShellIntegration`)
- Modify: `src/vtbackend/screen/Terminal.cpp` (`setMode`, `case DECMode::SemanticBlockProtocol` L4157-4164)
- Modify: `docs/vt-extensions/semantic-block-query.md` (L36-45, L62-66, L89-93, L157-162)
- Test: `src/vtbackend/shell/ShellIntegration_test.cpp` (`SemanticBlockProtocol.LineFlags` third section L237-250; `SemanticBlockProtocol.TrackerMetadata` L404-441; new case after `SemanticBlockProtocol.TokenChangesOnReEnable` L702); `src/vtbackend/screen/Screen_context_test.cpp` (delete the two legacy tracker cases by name — `Screen.osc3008 drives the semantic block tracker once it owns the session` and `Screen.osc3008 fires no callbacks during its first, undecided cycle`, pristine L579-612; Task 1.9's four cases follow them and stay)

**Interfaces:**
- Consumes: `CommandBlockStore::{forEachRecord, current}` (Task 1.2), `Terminal::commandBlocks()` (Task 1.8).
- Produces (reshaped, see contract note 9): `void SemanticBlockTracker::enable(CommandBlockId newestExisting)`, `void disable() noexcept`, `[[nodiscard]] bool isEnabled() const noexcept`, `[[nodiscard]] std::optional<Token> const& token() const noexcept`, `[[nodiscard]] bool validateToken(Token const&) const noexcept`, `[[nodiscard]] static Token generateToken()`, `[[nodiscard]] std::vector<CommandBlockRecord const*> finishedBlocks(CommandBlockStore const&, size_t count) const`, `[[nodiscard]] CommandBlockRecord const* inProgressBlock(CommandBlockStore const&) const noexcept`. Wire format of the 2034 replies unchanged.

**Test migration map (everything the tracker's tests pinned, and where it is asserted now):**

| Old assertion | New assertion |
|---|---|
| `LineFlags` §3: with 2034 off, `semanticBlockTracker().currentBlock()` is empty | with 2034 off the flags land, `commandBlocks().lastFinished()` holds `ls`, and `SBQUERY` replies `DCS > 0 b` |
| `TrackerMetadata` §1: `currentBlock()` finished, command `ls -la`, exit 0 | `commandBlocks().lastFinished()`: Finished, `ls -la`, Reported, exit 0 |
| `TrackerMetadata` §2: DECRM clears `currentBlock()` and `completedBlocks()` | DECRM clears the token and invalidates it; the store keeps its record |
| (new) | a block finished before DECSET 2034 is never reported; one started after is |
| `Screen_context_test` "drives the semantic block tracker…" (`currentBlock()->exitCode == 139`) | Task 1.9's "drives the command block store…" (`lastFinished()->exitCode == 139`, signal, finish event) |
| `Screen_context_test` "fires no callbacks during its first, undecided cycle" (`completedBlocks().empty()`) | Task 1.9's "feeds the store nothing during its first, undecided cycle" |
| `QueryLastCommand`, `QueryLastNCommands`, `QueryInProgress`, `NoCompletedCommands`, `TokenOnEnable`, `TokenInvalidatedOnDisable`, `QueryWithoutToken`, `QueryWithWrongToken`, `TokenChangesOnReEnable`, `DECRQM`, `QueryDisabled` | unchanged, and must keep passing against the store |

- [ ] **Step 1: Migrate the tests (they build against the legacy tracker; only `RecoveredCommandLineIsReported` fails until Step 6)**

In `src/vtbackend/shell/ShellIntegration_test.cpp`, replace the whole section

```cpp
    SECTION("What mode 2034 still gates: the tracker, not the flags")
```

(through its closing `}`) with

```cpp
    SECTION("What mode 2034 gates: the reader protocol, not the record store")
    {
        // The other half of the split above, pinned so it cannot quietly drift: with mode 2034 off the flags
        // land AND the terminal records the block -- its gutter, notifications and picker read the same
        // store -- but nothing is reported over the protocol.
        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("$ ");
        mc.writeToScreen("\033]133;C;cmdline_url=ls\033\\");
        mc.writeToScreen("file1\n");
        mc.writeToScreen("\033]133;D;0\033\\");

        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::OutputStart));
        REQUIRE(mc.terminal.commandBlocks().lastFinished() != nullptr);
        CHECK(mc.terminal.commandBlocks().lastFinished()->commandLine == "ls");

        mc.resetReplyData();
        mc.writeToScreen(SBQUERY(SBQueryType::LastCommand));
        mc.terminal.flushInput();
        CHECK(mc.replyData().contains("\033P>0b\033\\"));
    }
```

replace the whole `TEST_CASE("SemanticBlockProtocol.TrackerMetadata")` with

```cpp
TEST_CASE("SemanticBlockProtocol.StoreMetadata")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("The store keeps the command line and the exit code")
    {
        mc.writeToScreen(DECSM(2034));

        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("$ ");
        mc.writeToScreen("\033]133;C;cmdline_url=ls%20-la\033\\");
        mc.writeToScreen("file1\n");
        mc.writeToScreen("\033]133;D;0\033\\");

        auto const* block = mc.terminal.commandBlocks().lastFinished();
        REQUIRE(block != nullptr);
        CHECK(block->state == CommandBlockState::Finished);
        CHECK(block->commandLine == "ls -la");
        CHECK(block->commandLineSource == CommandLineSource::Reported);
        CHECK(block->exitCode == 0);
    }

    SECTION("Disabling mode 2034 ends the reader session but keeps the store")
    {
        auto const token = enableModeAndGetToken(mc);
        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("\033]133;C;cmdline_url=test\033\\");
        mc.writeToScreen("\033]133;D;0\033\\");

        mc.writeToScreen(DECRM(2034));

        auto const& tracker = mc.terminal.semanticBlockTracker();
        CHECK_FALSE(tracker.token().has_value());
        CHECK_FALSE(tracker.validateToken(token));
        CHECK(mc.terminal.commandBlocks().size() == 1);
    }
}
```

and after the closing `}` of `TEST_CASE("SemanticBlockProtocol.TokenChangesOnReEnable")` insert

```cpp

TEST_CASE("SemanticBlockProtocol.BlocksFromBeforeEnablingStayHidden")
{
    // The store records every block whether or not mode 2034 is on, but a reader must not be able to read
    // back commands that ran before it asked: enabling the mode starts a fresh reader session, as it did
    // when the tracker kept its own, initially empty, copy.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    simulateCommand(mc, "$ ", "secret", "out", 0);
    mc.writeToScreen("\033]133;A\033\\");

    auto const token = enableModeAndGetToken(mc);
    mc.resetReplyData();
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastCommand, 1, token));
    mc.terminal.flushInput();
    CHECK(mc.replyData().contains("\033P>0b\033\\"));

    // ...while a command that starts afterwards is reported.
    simulateCommand(mc, "$ ", "visible", "out", 0);
    mc.writeToScreen("\033]133;A\033\\");
    mc.resetReplyData();
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastNumberOfCommands, 5, token));
    mc.terminal.flushInput();
    CHECK(mc.replyData().contains("\"command\":\"visible\""));
    CHECK_FALSE(mc.replyData().contains("secret"));
}

TEST_CASE("SemanticBlockProtocol.RecoveredCommandLineIsReported")
{
    // tcsh has no clean hook for cmdline_url, so it marks ;B only; the store recovers what was typed from
    // the grid, and a reader of the protocol gets that rather than null.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);

    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\ls -la\r\n");
    mc.writeToScreen("\033]133;C\033\\file1\r\n\033]133;D;0\033\\");

    mc.resetReplyData();
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastCommand, 1, token));
    mc.terminal.flushInput();
    CHECK(mc.replyData().contains("\"command\":\"ls -la\""));
}
```

In `src/vtbackend/screen/Screen_context_test.cpp`, delete `TEST_CASE("Screen.osc3008 drives the semantic block tracker once it owns the session", "[context]")` and `TEST_CASE("Screen.osc3008 fires no callbacks during its first, undecided cycle", "[context]")` in full — Task 1.9 added their store-based successors.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "SemanticBlockProtocol.*"`
Expected: builds (the legacy tracker API still exists); FAIL in `SemanticBlockProtocol.RecoveredCommandLineIsReported` — the legacy tracker answers `"command":null`. The migrated cases and `BlocksFromBeforeEnablingStayHidden` already pass: they pin behaviour the rebase must preserve, and the store has been fed since Task 1.8.

- [ ] **Step 3: Rewrite `src/vtbackend/shell/SemanticBlockTracker.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandBlock.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vtbackend
{

/// SBQUERY (CSI > Ps ; Pn b) query type parameter values.
struct SBQueryType
{
    static constexpr unsigned LastCommand = 1;          ///< Last completed command block.
    static constexpr unsigned LastNumberOfCommands = 2; ///< Last N completed command blocks (Pn = count).
    static constexpr unsigned InProgress = 3;           ///< Current in-progress command.
};

/// The reader side of DEC mode 2034 (Semantic Block Reader Protocol): the session token, and which of the
/// terminal's command blocks a reader may see.
///
/// It keeps NO blocks of its own. The blocks are the terminal's CommandBlockStore, recorded whether or not
/// mode 2034 is on; this decides only what the protocol may report. A reader sees the blocks that started
/// after it enabled the mode -- what the protocol promised when this class kept its own copy -- so
/// enabling 2034 is never a way to read back commands that ran before.
class SemanticBlockTracker
{
  public:
    /// 64-bit session token represented as 4 x uint16_t for CSI parameter encoding.
    using Token = std::array<uint16_t, 4>;

    /// Starts a reader session: mints a fresh token and hides every block that exists already.
    /// @param newestExisting The newest block id the store holds right now; zero when it holds none.
    void enable(CommandBlockId newestExisting);

    /// Ends the reader session: the token is invalidated and nothing is reported any more.
    void disable() noexcept;

    /// @return Whether a reader session is active.
    [[nodiscard]] bool isEnabled() const noexcept;

    /// @return The current session token, while a session is active.
    [[nodiscard]] std::optional<Token> const& token() const noexcept;

    /// @param candidate The token a query carried.
    /// @return Whether it matches the current session's.
    [[nodiscard]] bool validateToken(Token const& candidate) const noexcept;

    /// @return A new random 64-bit token, from std::random_device.
    [[nodiscard]] static Token generateToken();

    /// The @p count most recent blocks this reader may see that FINISHED with a reported end, newest first.
    /// A block a newer prompt closed implicitly (a nested shell) is left out: it has no exit code to report.
    /// @param store The terminal's blocks.
    /// @param count How many at most.
    /// @return Pointers into @p store, valid until it next changes.
    [[nodiscard]] std::vector<CommandBlockRecord const*> finishedBlocks(CommandBlockStore const& store,
                                                                        size_t count) const;

    /// @param store The terminal's blocks.
    /// @return The block in progress (prompting or running), if this reader may see it, else nullptr.
    [[nodiscard]] CommandBlockRecord const* inProgressBlock(CommandBlockStore const& store) const noexcept;

  private:
    /// Whether @p record started after this reader session did.
    [[nodiscard]] bool isVisible(CommandBlockRecord const& record) const noexcept;

    std::optional<Token> _token;
    CommandBlockId _hiddenThrough {}; ///< Blocks up to this id existed before the session began.
};

} // namespace vtbackend
```

- [ ] **Step 4: Rewrite `src/vtbackend/shell/SemanticBlockTracker.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/SemanticBlockTracker.hpp>

#include <algorithm>
#include <random>

namespace vtbackend
{

void SemanticBlockTracker::enable(CommandBlockId newestExisting)
{
    _token = generateToken();
    _hiddenThrough = newestExisting;
}

void SemanticBlockTracker::disable() noexcept
{
    _token.reset();
}

bool SemanticBlockTracker::isEnabled() const noexcept
{
    return _token.has_value();
}

std::optional<SemanticBlockTracker::Token> const& SemanticBlockTracker::token() const noexcept
{
    return _token;
}

bool SemanticBlockTracker::validateToken(Token const& candidate) const noexcept
{
    return _token.has_value() && *_token == candidate;
}

SemanticBlockTracker::Token SemanticBlockTracker::generateToken()
{
    auto rd = std::random_device {};
    auto token = Token {};
    for (auto& part: token)
        part = static_cast<uint16_t>(rd());
    return token;
}

std::vector<CommandBlockRecord const*> SemanticBlockTracker::finishedBlocks(CommandBlockStore const& store,
                                                                            size_t count) const
{
    auto blocks = std::vector<CommandBlockRecord const*> {};
    if (!isEnabled())
        return blocks;

    store.forEachRecord([&](CommandBlockRecord const& record) {
        if (isVisible(record) && record.state == CommandBlockState::Finished
            && record.end == CommandBlockEnd::Reported)
            blocks.push_back(&record);
    });
    std::ranges::reverse(blocks);
    if (blocks.size() > count)
        blocks.resize(count);
    return blocks;
}

CommandBlockRecord const* SemanticBlockTracker::inProgressBlock(CommandBlockStore const& store) const noexcept
{
    auto const* current = store.current();
    return (isEnabled() && current && isVisible(*current)) ? current : nullptr;
}

bool SemanticBlockTracker::isVisible(CommandBlockRecord const& record) const noexcept
{
    return record.id > _hiddenThrough;
}

} // namespace vtbackend
```

- [ ] **Step 5: Drive it from `setMode`** — in `src/vtbackend/screen/Terminal.cpp` replace

```cpp
        case DECMode::SemanticBlockProtocol:
            _semanticBlockTracker.setEnabled(enable);
            if (enable)
            {
                auto const& t = *_semanticBlockTracker.token();
                reply("\033P>2034;1b{};{};{};{}\033\\", t[0], t[1], t[2], t[3]);
            }
            break;
```

with

```cpp
        case DECMode::SemanticBlockProtocol:
            if (enable)
            {
                // A fresh reader session sees only blocks that start from here on.
                auto newest = CommandBlockId {};
                _commandBlocks.forEachRecord([&newest](CommandBlockRecord const& record) { newest = record.id; });
                _semanticBlockTracker.enable(newest);
                auto const& t = *_semanticBlockTracker.token();
                reply("\033P>2034;1b{};{};{};{}\033\\", t[0], t[1], t[2], t[3]);
            }
            else
                _semanticBlockTracker.disable();
            break;
```

- [ ] **Step 6: Answer queries from the store** — in `src/vtbackend/screen/Screen.hpp` replace the forward declarations

```cpp
class SemanticBlockTracker;
class SixelImageBuilder;
class Terminal;
struct CommandBlockInfo;
struct Settings;
```

with

```cpp
class SixelImageBuilder;
class Terminal;
struct CommandBlockRecord;
struct Settings;
```

and the declarations

```cpp
    void handleInProgressQuery(SemanticBlockTracker const& tracker);
    void handleCompletedBlocksQuery(SemanticBlockTracker const& tracker,
                                    std::deque<CommandBlockInfo> const& completedBlocks,
                                    unsigned queryType,
                                    int count);
```

with

```cpp
    void handleInProgressQuery(CommandBlockRecord const* block);
    void handleCompletedBlocksQuery(std::vector<CommandBlockRecord const*> const& blocks);
```

and add `#include <vector>` after `#include <string_view>` in Screen.hpp's standard-library group (the header names `std::vector` itself now).

In `src/vtbackend/screen/Screen.cpp` replace `formatBlockJson`

```cpp
    /// Formats a single CommandBlockInfo as a JSON object.
    std::string formatBlockJson(CommandBlockInfo const& block,
                                std::string_view prompt,
                                std::string_view output,
                                int outputLineCount)
    {
        return std::format(
            R"({{"command":{},"prompt":{},"output":{},"exitCode":{},"finished":{},"outputLineCount":{}}})",
            block.commandLine ? jsonEscape(*block.commandLine) : "null",
            jsonEscape(prompt),
            jsonEscape(output),
            block.exitCode,
            block.finished ? "true" : "false",
            outputLineCount);
    }
```

with

```cpp
    /// Formats a single command block as a JSON object.
    std::string formatBlockJson(CommandBlockRecord const& block,
                                std::string_view prompt,
                                std::string_view output,
                                int outputLineCount)
    {
        return std::format(
            R"({{"command":{},"prompt":{},"output":{},"exitCode":{},"finished":{},"outputLineCount":{}}})",
            block.commandLineSource != CommandLineSource::None ? jsonEscape(block.commandLine) : "null",
            jsonEscape(prompt),
            jsonEscape(output),
            block.exitCode.value_or(-1),
            block.state == CommandBlockState::Finished ? "true" : "false",
            outputLineCount);
    }
```

In `handleSemanticBlockQuery`, replace from `auto const queryType = seq.paramOr(0, SBQueryType::LastCommand);` to the end of the function with

```cpp
    auto const queryType = seq.paramOr(0, SBQueryType::LastCommand);
    auto const count = seq.paramOr(1, 1); // Pn: count (default 1)
    auto const& blocks = _terminal->commandBlocks();

    if (queryType == SBQueryType::InProgress)
    {
        handleInProgressQuery(tracker.inProgressBlock(blocks));
        return;
    }

    auto const requestedCount = (queryType == SBQueryType::LastCommand) ? 1 : std::max(count, 1);
    handleCompletedBlocksQuery(tracker.finishedBlocks(blocks, static_cast<size_t>(requestedCount)));
}
```

In `handleInProgressQuery` replace the head

```cpp
void Screen::handleInProgressQuery(SemanticBlockTracker const& tracker)
{
    auto const& currentBlock = tracker.currentBlock();
    if (!currentBlock || currentBlock->finished)
    {
        reply(SBQueryResponseDisabled);
        return;
    }
```

with

```cpp
void Screen::handleInProgressQuery(CommandBlockRecord const* block)
{
    if (!block)
    {
        reply(SBQueryResponseDisabled);
        return;
    }
```

and `auto const json = formatBlockJson(*currentBlock, promptText, outputText, outputLineCount);` with `auto const json = formatBlockJson(*block, promptText, outputText, outputLineCount);`.

Replace the head of `handleCompletedBlocksQuery`, from its signature through the closing `}` of `if (blocks.empty()) { … }`,

```cpp
void Screen::handleCompletedBlocksQuery(SemanticBlockTracker const& tracker,
                                        std::deque<CommandBlockInfo> const& completedBlocks,
                                        unsigned queryType,
                                        int count)
{
    auto const requestedCount = (queryType == SBQueryType::LastCommand) ? 1 : std::max(count, 1);

    if (completedBlocks.empty())
    {
        // Check if there's a finished current block not yet pushed.
        if (!tracker.currentBlock() || !tracker.currentBlock()->finished)
        {
            reply(SBQueryResponseDisabled);
            return;
        }
    }

    // Collect blocks to return (most recent first, then reverse for JSON output).
    auto blocks = std::vector<CommandBlockInfo const*> {};

    // Include the current block if finished.
    if (tracker.currentBlock() && tracker.currentBlock()->finished)
        blocks.push_back(&*tracker.currentBlock());

    // Add from completed blocks (back = most recent).
    for (auto it = completedBlocks.rbegin();
         it != completedBlocks.rend() && std::cmp_less(blocks.size(), requestedCount);
         ++it)
        blocks.push_back(&*it);

    if (blocks.empty())
    {
        reply(SBQueryResponseDisabled);
        return;
    }

    // Reconstruct each block's text from the OSC 133 marks the shell left in the grid. The tracker knows
    // HOW MANY blocks there are and what their metadata is; the grid is what still holds their text.
```

with

```cpp
void Screen::handleCompletedBlocksQuery(std::vector<CommandBlockRecord const*> const& blocks)
{
    if (blocks.empty())
    {
        reply(SBQueryResponseDisabled);
        return;
    }

    // Reconstruct each block's text from the OSC 133 marks the shell left in the grid. The store knows
    // HOW MANY blocks there are and what their metadata is; the grid is what still holds their text.
```

(the rest of the function — `scanCommandBlocksBackward` and the JSON assembly over `blocks[i]` — is unchanged).

- [ ] **Step 7: Stop feeding the tracker** — in `src/vtbackend/screen/Screen.cpp` delete these six lines (they now refer to removed members):
  - in `synthesizeSemanticMarks`: `_terminal->semanticBlockTracker().promptStart();`, `_terminal->semanticBlockTracker().commandOutputStart(std::nullopt);`, `_terminal->semanticBlockTracker().commandFinished(exitCode);`
  - in `processShellIntegration`: `_terminal->semanticBlockTracker().promptStart();` (case `A`), `_terminal->semanticBlockTracker().commandOutputStart(reported);` (case `C`), `_terminal->semanticBlockTracker().commandFinished(exitCode);` (case `D`)

- [ ] **Step 8: Document the reader session** — in `docs/vt-extensions/semantic-block-query.md` replace

```markdown
When enabled:
- The terminal tracks semantic zones from OSC 133 sequences using line flags and internal metadata.
```

with

```markdown
When enabled:
- The terminal reports the command blocks that start from this point on. Contour records every shell's
  command blocks whether or not this mode is on (its gutter, notifications and recent-commands picker
  read them), but a reader only ever sees blocks that began after it enabled the mode, so enabling it is
  never a way to read back commands that ran earlier.
```

replace `- All tracked semantic block data is discarded.` with `- Nothing is reported any more; blocks recorded so far stay hidden even if the mode is enabled again.`; replace `3. **On DECRST 2034**: The token is invalidated along with all tracked data.` with `3. **On DECRST 2034**: The token is invalidated, and the blocks recorded so far are hidden from every later session.`; after the query-type table (the row `| 3  | Current in-progress command     | ignored             |`) add the paragraph

```markdown

A *completed* block is one the shell finished with OSC 133;D (or OSC 3008 `end=`). A block that a newer
prompt superseded without one -- the prompt of a nested shell, such as inside `ssh` -- is not reported.
```

and in the Fields table replace the `command` and `exitCode` rows with

```markdown
| `command`         | string or null    | The command line from OSC 133;C `cmdline_url`, or -- when the shell sent none but marked its prompt end with OSC 133;B -- the text typed after the prompt. `null` when neither is known. Longer than 4096 bytes it is cut at a character boundary and ends in `…`. |
| `exitCode`        | integer           | From OSC 133;D (or OSC 3008 `end=`). `-1` if unknown.            |
```

- [ ] **Step 9: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "SemanticBlockProtocol.*"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[context]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe`
Expected: zero warnings; `All tests passed` — every pre-existing 2034 case (`QueryLastCommand` … `TokenChangesOnReEnable`) unchanged and green, plus `BlocksFromBeforeEnablingStayHidden`.

- [ ] **Step 10: Format and commit**

Run: `clang-format -i src/vtbackend/shell/SemanticBlockTracker.hpp src/vtbackend/shell/SemanticBlockTracker.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.cpp src/vtbackend/shell/ShellIntegration_test.cpp src/vtbackend/screen/Screen_context_test.cpp`

```bash
git add src/vtbackend/shell/SemanticBlockTracker.hpp src/vtbackend/shell/SemanticBlockTracker.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.cpp src/vtbackend/shell/ShellIntegration_test.cpp src/vtbackend/screen/Screen_context_test.cpp docs/vt-extensions/semantic-block-query.md
git commit -F - <<'EOF'
vtbackend: rebase the mode 2034 reader onto the block store

SemanticBlockTracker keeps the session token and the query encoding and
reads blocks from the terminal's CommandBlockStore instead of its own deque.
A reader still sees only blocks that started after it enabled the mode;
mode 2034 gates the protocol, never what the terminal records.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.11: Remove `ShellIntegration` / `NullShellIntegration`

**Files:**
- Delete: `src/vtbackend/shell/ShellIntegration.hpp`
- Modify: `src/vtbackend/CMakeLists.txt` (drop `shell/ShellIntegration.hpp` from `vtbackend_HEADERS`)
- Modify: `src/vtbackend/screen/Terminal.hpp` (include L25; accessors L1362-1367; member L2861)
- Modify: `src/vtbackend/screen/Terminal.cpp` (ctor init list L229-230)
- Modify: `src/vtbackend/screen/Screen.cpp` (`processShellIntegration` cases `A`, `B`, `C`, `D`; `synthesizeSemanticMarks`)
- Modify: `src/vtbackend/shell/MarkArbiter.hpp` (doc L25-26, L115-120)
- Modify: `docs/vt-extensions/osc-133-shell-integration.md` (L25, `click_events`)
- Test: `src/vtbackend/shell/ShellIntegration_test.cpp` (include L4; `MockShellIntegration` L78-108; `TEST_CASE("ShellIntegration.OSC_133")` L138-196)

**Interfaces:**
- Consumes: Tasks 1.8-1.10.
- Produces: removal of `ShellIntegration`, `NullShellIntegration`, `Terminal::shellIntegration()`, `Terminal::setShellIntegration()` (spec §4.5). No production implementation of the interface ever existed (`git grep -n "public ShellIntegration\|: ShellIntegration" -- src` lists only the test's mock).

**Migration of `ShellIntegration.OSC_133` (each callback assertion → store/flag assertion):**

| Section | Old (mock callback) | New |
|---|---|---|
| A | `promptStartCount == 1`, clickEvents false | one record, Prompting; Marked on row 0 |
| A with click_events | `lastPromptStartClickEvents == true`, Marked | one record, Marked — `click_events=` is accepted and ignored (click-to-move is a follow-up, spec §1.3) |
| B | `promptEndCount == 1` | PromptEnd on row 0; no record (a ;B with no ;A opens nothing) |
| C | `commandOutputStartCount == 1`, url nullopt | a record minted by ;C, Running, `CommandLineSource::None` |
| C with cmdline_url | url == "foo bar" | `commandLine == "foo bar"`, Reported |
| D | `commandFinishedCount == 1`, exit 0 | after a ;C: finished, exit 0, one finish event |
| D with exit code | exit 123 | after a ;C: exit 123 |
| (new) D with nothing running | — | ignored: no record, no event, CommandEnd still stamped |

- [ ] **Step 1: Migrate the test** — in `src/vtbackend/shell/ShellIntegration_test.cpp` delete `#include <vtbackend/shell/ShellIntegration.hpp>` and the whole `class MockShellIntegration: public ShellIntegration { … };`, and replace the whole `TEST_CASE("ShellIntegration.OSC_133")` with

```cpp
TEST_CASE("ShellIntegration.OSC_133")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const& blocks = mc.terminal.commandBlocks();

    SECTION("A: Prompt Start")
    {
        mc.writeToScreen("\033]133;A\033\\");
        REQUIRE(blocks.size() == 1);
        REQUIRE(blocks.current() != nullptr);
        CHECK(blocks.current()->state == CommandBlockState::Prompting);
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    }

    SECTION("A: Prompt Start with click_events")
    {
        // click_events= asks for click-to-move in the prompt, a follow-up rather than part of a block: the
        // parameter is accepted and ignored, and the prompt is recorded exactly as without it.
        mc.writeToScreen("\033]133;A;click_events=1\033\\");
        CHECK(blocks.size() == 1);
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    }

    SECTION("B: Prompt End")
    {
        mc.writeToScreen("\033]133;B\033\\");
        // The border lives on the line; the store has nothing to record for a prompt it never saw start.
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::PromptEnd));
        CHECK(blocks.size() == 0);
    }

    SECTION("C: Command Output Start")
    {
        // A ;C with no ;A before it -- a shell integration sourced halfway through a session -- mints the
        // record it needs.
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(blocks.current() != nullptr);
        CHECK(blocks.current()->state == CommandBlockState::Running);
        CHECK(blocks.current()->commandLineSource == CommandLineSource::None);
        CHECK(blocks.current()->commandLine.empty());
    }

    SECTION("C: Command Output Start with cmdline_url")
    {
        // "foo%20bar" unescapes to "foo bar"
        mc.writeToScreen("\033]133;C;cmdline_url=foo%20bar\033\\");
        REQUIRE(blocks.current() != nullptr);
        CHECK(blocks.current()->commandLine == "foo bar");
        CHECK(blocks.current()->commandLineSource == CommandLineSource::Reported);
    }

    SECTION("D: Command Finished")
    {
        mc.writeToScreen("\033]133;C\033\\");
        mc.writeToScreen("\033]133;D\033\\");
        REQUIRE(blocks.lastFinished() != nullptr);
        CHECK(blocks.lastFinished()->exitCode == 0);
        CHECK(mc.finishedCommandBlocks.size() == 1);
    }

    SECTION("D: Command Finished with exit code")
    {
        mc.writeToScreen("\033]133;C\033\\");
        mc.writeToScreen("\033]133;D;123\033\\");
        REQUIRE(blocks.lastFinished() != nullptr);
        CHECK(blocks.lastFinished()->exitCode == 123);
    }

    SECTION("D: with no command running is ignored")
    {
        // tcsh emits ;D unconditionally, and a nested shell's exit leaves one behind: neither finished a
        // command this terminal saw start.
        mc.writeToScreen("\033]133;D;1\033\\");
        CHECK(blocks.size() == 0);
        CHECK(mc.finishedCommandBlocks.empty());
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::CommandEnd));
    }
}
```

- [ ] **Step 2: Run it** — it already passes against the store (Tasks 1.8-1.10 did the work; this task only removes dead weight):

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "ShellIntegration.OSC_133"`
Expected: `All tests passed`.

- [ ] **Step 3: Delete the interface** — `git rm src/vtbackend/shell/ShellIntegration.hpp`; in `src/vtbackend/CMakeLists.txt` delete the line `    shell/ShellIntegration.hpp`.

In `src/vtbackend/screen/Terminal.hpp` delete `#include <vtbackend/shell/ShellIntegration.hpp>`, delete

```cpp
    [[nodiscard]] ShellIntegration& shellIntegration() noexcept { return *_shellIntegration; }
    [[nodiscard]] ShellIntegration const& shellIntegration() const noexcept { return *_shellIntegration; }
    void setShellIntegration(std::unique_ptr<ShellIntegration> newShellIntegration)
    {
        _shellIntegration = std::move(newShellIntegration);
    }

```

and delete the member line `    std::unique_ptr<ShellIntegration> _shellIntegration;`.

In `src/vtbackend/screen/Terminal.cpp` replace

```cpp
    _inputHandler { _viCommands, ViMode::Insert },
    _shellIntegration { std::make_unique<NullShellIntegration>() }
{
```

with

```cpp
    _inputHandler { _viCommands, ViMode::Insert }
{
```

- [ ] **Step 4: Drop the calls** — in `src/vtbackend/screen/Screen.cpp`, `processShellIntegration`, replace case `'A'`

```cpp
        case 'A': {
            setMark();
            bool clickEvents = false;
            auto const params = seq.intermediateCharacters().substr(1);
            forEachKeyValue(params, [&](std::string_view key, std::string_view value) {
                if (key == "click_events" && value == "1")
                    clickEvents = true;
            });
            _terminal->shellIntegration().promptStart(clickEvents);
            if (isPrimaryPage())
                _terminal->beginCommandBlock();
            _terminal->autoCollapseOnNewPrompt();
            break;
        }
```

with

```cpp
        case 'A': {
            // `click_events=1` (click-to-move in the prompt) is accepted and ignored: prompt editing, not
            // blocks, and a follow-up of its own.
            setMark();
            if (isPrimaryPage())
                _terminal->beginCommandBlock();
            _terminal->autoCollapseOnNewPrompt();
            break;
        }
```

in case `'B'` delete the line `            _terminal->shellIntegration().promptEnd();` (and the blank line before it); in case `'C'` replace

```cpp
            // The legacy observers hear only what the SHELL reported, exactly as before.
            auto const reported = source == CommandLineSource::Reported ? commandLine : std::nullopt;
            _terminal->shellIntegration().commandOutputStart(reported);
            if (isPrimaryPage())
```

with

```cpp
            if (isPrimaryPage())
```

in case `'D'` delete `            _terminal->shellIntegration().commandFinished(exitCode);`; and in `synthesizeSemanticMarks` replace

```cpp
            if (notify)
            {
                _terminal->shellIntegration().promptStart();
                _terminal->autoCollapseOnNewPrompt();
            }
```

with

```cpp
            if (notify)
                _terminal->autoCollapseOnNewPrompt();
```

and delete the two blocks that are now empty of everything but the removed call:

```cpp
                if (notify)
                {
                    // No cmdline: the systemd shim sends none, and 3008 may FILL a command line but
                    // never overwrite one OSC 133;C already supplied.
                    _terminal->shellIntegration().commandOutputStart(std::nullopt);
                }
```

and

```cpp
                if (notify)
                {
                    auto const exitCode = command.outcome.asShellExitCode();
                    _terminal->shellIntegration().commandFinished(exitCode);
                }
```

(the `feedsBlocks` branches added in Task 1.9 stay; the store's `commandStarted` already never overwrites a command line, which is what the deleted comment guarded).

- [ ] **Step 5: Correct the arbiter's docs** — in `src/vtbackend/shell/MarkArbiter.hpp` replace

```cpp
/// user action at all. Both would otherwise stamp LineFlag::Marked / OutputStart / CommandEnd and both
/// would drive the ShellIntegration callbacks, so exactly one of them is the source of record.
```

with

```cpp
/// user action at all. Both would otherwise stamp LineFlag::Marked / OutputStart / CommandEnd and both
/// would feed the CommandBlockStore, so exactly one of them is the source of record.
```

and

```cpp
    /// Whether OSC 3008 may drive the ShellIntegration callbacks and the SemanticBlockTracker.
    ///
    /// Strictly narrower than @ref contextMayMark: a flag stamped twice is one bit, but promptStart()
    /// delivered twice is two notifications -- and SemanticBlockTracker::commandOutputStart() assigns
    /// its command line unconditionally, so a 3008 command context, which carries no cmdline= at all
    /// under systemd, would otherwise clobber a good cmdline_url from OSC 133;C.
```

with

```cpp
    /// Whether OSC 3008 may open, start and finish command blocks in the CommandBlockStore.
    ///
    /// Strictly narrower than @ref contextMayMark: a flag stamped twice is one bit, but a prompt start
    /// delivered twice is two blocks -- the second discarding the first -- and a 3008 command context
    /// racing OSC 133;C would start the same command twice.
```

- [ ] **Step 6: Correct the protocol doc** — in `docs/vt-extensions/osc-133-shell-integration.md` replace the `click_events=1` bullet with

```markdown
* `click_events=1`: Optional. Accepted for compatibility and currently ignored: Contour does not yet move the cursor on a click inside the prompt.
```

- [ ] **Step 7: Run everything that touched the interface**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test vthost_test vtconformance_test contour_gui_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe` and `out/build/clangcl-debug/bin/vthost_test.exe`
Expected: zero warnings; `All tests passed`. `git grep -n "NullShellIntegration\|shellIntegration()\|setShellIntegration\|shell/ShellIntegration.hpp" -- src` prints nothing (remaining `ShellIntegration` hits are the `MarkOwner::ShellIntegration` enumerator, `observedShellIntegration` and test names, which name the protocol owner, not the deleted class).

- [ ] **Step 8: Format and commit**

Run: `clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/shell/MarkArbiter.hpp src/vtbackend/shell/ShellIntegration_test.cpp`

```bash
git add src/vtbackend/CMakeLists.txt src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/shell/MarkArbiter.hpp src/vtbackend/shell/ShellIntegration_test.cpp docs/vt-extensions/osc-133-shell-integration.md
git commit -F - <<'EOF'
vtbackend: remove the unused ShellIntegration interface

It never had a production implementation; the command block store is now
the one consumer of OSC 133. Its tests assert against the store and the
line flags instead of a mock's callback counters.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.12: Re-record block heads after row identity is destroyed

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp` (private `syncCommandBlockHeads` after `sampleBatchClock`; `_commandBlocks` becomes `mutable`, plus a generation member)
- Modify: `src/vtbackend/screen/Terminal.cpp` (`commandBlocks()` overloads; new `syncCommandBlockHeads`)
- Test: `src/vtbackend/screen/Terminal_blocks_test.cpp` (append)

**Interfaces:**
- Consumes: `CommandBlockStore::{find, updateHeadPosition, forEachRecord}` (Task 1.3); `Grid::{stableIdGeneration, stableRangeFloor, stableLineIdOf, addressableTop, lineOffsetOf}`.
- Produces: the guarantee every C1 consumer relies on — after any `Terminal::commandBlocks()` call, every record's `headIdGeneration` equals the primary grid's `stableIdGeneration()`, and its `headStableId` names the block's head row, or lies below `stableRangeFloor()` when that row was evicted — except on a daemon mirror, where a record whose head row is not yet written carries `vthost::UnknownHeadGeneration` and consumers treat it as having no head. Lazy, keyed on `Grid::stableIdGeneration()` (one integer compare when nothing moved), the same shape as `Terminal::refreshFoldState()`.

- [ ] **Step 1: Write the failing tests** (Review Focus #1) — append to `src/vtbackend/screen/Terminal_blocks_test.cpp`:

```cpp
// {{{ heads survive a reflow

namespace
{

/// One logical line of the primary grid as a reader sees it: its text, and the block and birth stamp
/// its HEAD carries -- what a reflow re-chops but must not change.
struct LogicalLine
{
    std::string text;
    CommandBlockId block {};
    uint32_t bornAt = 0;

    bool operator==(LogicalLine const&) const = default;
};

/// Every logical line from the top of the scrollback to the bottom of the page; blank rows the cursor
/// never reached are left out, since a reflow adds and drops those freely.
[[nodiscard]] std::vector<LogicalLine> logicalLines(Terminal& terminal)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto lines = std::vector<LogicalLine> {};
    for (auto const row: std::views::iota(unbox<int>(grid.addressableTop()), unbox<int>(grid.pageSize().lines)))
    {
        auto const offset = LineOffset::cast_from(row);
        auto const& line = grid.lineAt(offset);
        if (line.isFlagEnabled(LineFlag::Wrapped) && !lines.empty())
            lines.back().text += grid.lineText(offset);
        else
            lines.push_back(
                LogicalLine { .text = grid.lineText(offset), .block = line.blockId(), .bornAt = line.bornAt() });
    }
    for (auto& line: lines)
        line.text.resize(core::trimRight(line.text).size());
    std::erase_if(lines, [](LogicalLine const& line) { return line.text.empty() && line.bornAt == 0; });
    return lines;
}

/// A command still printing when the window is resized: a prompt at t=10s, then eight 30-column lines at
/// t=15s on a 20-column page, so every one of them wraps.
CommandBlockId runLongCommand(MockTerm<>& mc)
{
    mc.wallClock.advance(10s);
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\make\r\n\033]133;C\033\\");
    mc.wallClock.advance(5s);
    for (auto const i: std::views::iota(0, 8))
        mc.writeToScreen(std::format("{:-<30}\r\n", i));
    return mc.terminal.commandBlocks().currentId();
}

/// The record's head is re-recorded in the grid's current generation and names the prompt row.
void checkHeadIsThePrompt(MockTerm<>& mc, CommandBlockId id)
{
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const* record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    CHECK(record->headIdGeneration == grid.stableIdGeneration());
    auto const head = grid.lineOffsetOf(record->headStableId);
    REQUIRE(head.has_value());
    CHECK(grid.lineAt(*head).isFlagEnabled(LineFlag::Marked));
    CHECK(grid.lineTextTrimmed(*head) == "$ make");
}

} // namespace

TEST_CASE("Terminal.blocks.idsTimesAndHeadsSurviveAColumnResizeMidCommand", "[terminal][semanticblocks]")
{
    // Review Focus #1: the window is resized while a long command is still printing. Reflow rebuilds the
    // ring and destroys stable row identity; the running block must keep its id on every row, every row
    // its birth time, and the record its head -- re-recorded in the new id generation.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    auto const id = runLongCommand(mc);
    REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
    REQUIRE(mc.terminal.commandBlocks().current()->state == CommandBlockState::Running);

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generationBefore = grid.stableIdGeneration();
    auto const before = logicalLines(mc.terminal);
    REQUIRE(before.front().text == "$ make");

    SECTION("narrowing")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
        REQUIRE(grid.stableIdGeneration() != generationBefore);

        CHECK(logicalLines(mc.terminal) == before);
        for (auto const row: std::views::iota(unbox<int>(grid.addressableTop()), unbox<int>(grid.pageSize().lines)))
        {
            auto const offset = LineOffset::cast_from(row);
            if (grid.lineTextTrimmed(offset).empty())
                continue;
            CHECK(grid.lineAt(offset).blockId() == id);
            CHECK((grid.lineAt(offset).bornAt() == 11 || grid.lineAt(offset).bornAt() == 16));
        }
        checkHeadIsThePrompt(mc, id);
    }

    SECTION("narrowing and widening again")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(25) });

        CHECK(logicalLines(mc.terminal) == before);
        // A widening rebuilds each logical line from its head, so every chunk carries the head's stamp.
        auto headStamp = uint32_t { 0 };
        for (auto const row: std::views::iota(unbox<int>(grid.addressableTop()), unbox<int>(grid.pageSize().lines)))
        {
            auto const& line = grid.lineAt(LineOffset::cast_from(row));
            if (!line.isFlagEnabled(LineFlag::Wrapped))
                headStamp = line.bornAt();
            else
                CHECK(line.bornAt() == headStamp);
        }
        checkHeadIsThePrompt(mc, id);
    }
}

TEST_CASE("Terminal.blocks.aHeadEvictedBeforeTheResizeIsRecordedAsEvicted", "[terminal][semanticblocks]")
{
    // The prompt row is gone before the resize; afterwards the record must still say so, in the NEW
    // generation's terms -- an id below the floor -- rather than adopt whatever row is oldest now.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(2) };
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\make\r\n\033]133;C\033\\");
    for (auto const i: std::views::iota(0, 10))
        mc.writeToScreen(std::format("{:-<18}\r\n", i));
    auto const id = mc.terminal.commandBlocks().currentId();
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(mc.terminal.commandBlocks().find(id) != nullptr);
    REQUIRE_FALSE(grid.lineOffsetOf(mc.terminal.commandBlocks().find(id)->headStableId).has_value());

    auto const generationBefore = grid.stableIdGeneration();
    mc.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(10) }); // every 18-column row now wraps
    REQUIRE(grid.stableIdGeneration() != generationBefore);

    auto const* record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    CHECK(record->headIdGeneration == grid.stableIdGeneration());
    CHECK(record->headStableId < grid.stableRangeFloor());
}

TEST_CASE("Terminal.blocks.aBlockStartedAfterTheResizeKeepsTheHeadItWasGiven", "[terminal][semanticblocks]")
{
    // Guards the rescan's skip: a record minted in the new generation already holds a valid head.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    mc.writeToScreen(std::string(30, 'x') + "\r\n"); // a wrapped line, so the resize rebuilds the ring
    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
    mc.writeToScreen("\033]133;A\033\\$ ");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    auto const& grid = mc.terminal.primaryScreen().grid();
    CHECK(record->headIdGeneration == grid.stableIdGeneration());
    auto const head = grid.lineOffsetOf(record->headStableId);
    REQUIRE(head.has_value());
    CHECK(grid.lineTextTrimmed(*head) == "$");
}

// }}}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "Terminal.blocks.*"`
Expected: FAIL — in the first two cases `record->headIdGeneration == grid.stableIdGeneration()` is false (the record still holds the pre-resize generation); the row id/stamp checks and `aBlockStartedAfterTheResize…` already pass (Tasks 1.5 and 1.8).

- [ ] **Step 3: Declare the rescan** — in `src/vtbackend/screen/Terminal.hpp`, after the `sampleBatchClock()` declaration insert:

```cpp

    /// Re-records every block's head after row identity was destroyed (a column resize, a history-limit
    /// change, a reset): a no-op unless Grid::stableIdGeneration() moved since the last call, and it fronts
    /// both commandBlocks() overloads, so no reader ever sees a head from a dead generation. The same lazy
    /// shape as refreshFoldState().
    void syncCommandBlockHeads() const noexcept;
```

and replace the member

```cpp
    CommandBlockStore _commandBlocks { _settings.commandBlockLimits, _clocks.steady, _clocks.wall };
```

with

```cpp
    /// Mutable for the reason _foldState is: syncCommandBlockHeads() reconciles it lazily from const readers.
    mutable CommandBlockStore _commandBlocks { _settings.commandBlockLimits, _clocks.steady, _clocks.wall };

    /// The Grid::stableIdGeneration() the heads in _commandBlocks were last recorded in.
    mutable uint64_t _commandBlockHeadsGeneration = 0;
```

- [ ] **Step 4: Implement it** — in `src/vtbackend/screen/Terminal.cpp` replace

```cpp
CommandBlockStore& Terminal::commandBlocks() noexcept
{
    return _commandBlocks;
}

CommandBlockStore const& Terminal::commandBlocks() const noexcept
{
    return _commandBlocks;
}
```

with

```cpp
CommandBlockStore& Terminal::commandBlocks() noexcept
{
    syncCommandBlockHeads();
    return _commandBlocks;
}

CommandBlockStore const& Terminal::commandBlocks() const noexcept
{
    syncCommandBlockHeads();
    return _commandBlocks;
}

void Terminal::syncCommandBlockHeads() const noexcept
{
    auto const& grid = primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    if (generation == _commandBlockHeadsGeneration)
        return;
    _commandBlockHeadsGeneration = generation;

    // One walk of the rows that hold valid data, top down. A block's TOPMOST surviving row is its head when
    // it carries the mark the block began with -- ;A's Marked, or ;C's OutputStart for a block a ;C minted.
    // When it carries neither, the head was evicted before the rebuild, and the record says so with an id
    // below the new floor, which is what "evicted" means everywhere else. (A block whose prompt rows went
    // but whose ;C row survived is re-headed at that row: still a row of the block.)
    constexpr auto HeadMarks = LineFlags { LineFlag::Marked, LineFlag::OutputStart };
    auto const evicted = grid.stableRangeFloor() - 1;
    auto previous = CommandBlockId {};
    for (auto const row: std::views::iota(unbox<int>(grid.addressableTop()), unbox<int>(grid.pageSize().lines)))
    {
        auto const offset = LineOffset::cast_from(row);
        auto const& line = grid.lineAt(offset);
        auto const id = line.blockId();
        if (!id || id == previous)
            continue;
        previous = id;

        auto const* record = _commandBlocks.find(id);
        if (!record || record->headIdGeneration == generation)
            continue; // unknown, minted after the rebuild, or already placed by a row above
        _commandBlocks.updateHeadPosition(
            id, line.isFlagEnabled(HeadMarks) ? grid.stableLineIdOf(offset) : evicted, generation);
    }

    // Records none of whose rows survived.
    _commandBlocks.forEachRecord([&](CommandBlockRecord const& record) {
        if (record.headIdGeneration != generation)
            _commandBlocks.updateHeadPosition(record.id, evicted, generation);
    });
}
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[folding]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_blocks_test.cpp`

```bash
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_blocks_test.cpp
git commit -F - <<'EOF'
vtbackend: re-record block heads after row identity is destroyed

A column resize rebuilds the ring and renames every row. commandBlocks()
now re-records each block's head lazily, keyed on the grid's stable-id
generation: the topmost surviving row of the block when it carries the
block's opening mark, otherwise an id below the floor (evicted). Row ids and
birth times survive the reflow on every row, heads are re-recorded.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.13: Configuration — `command_blocks.max_records`

**Files:**
- Modify: `src/contour/config/Config.hpp` (struct after `OscContextConfig` ~L407; member after `oscContext` ~L1309; `loadFromEntry` declaration after the `OscContextConfig` one ~L1627; `Writer::format` overload after the `OscContextConfig` one ~L1938)
- Modify: `src/contour/config/Config.cpp` (`emulationSettings` after the folding mapping ~L471; `loadFromEntry("command_blocks", …)` after `osc_context` ~L1057; the overload after `loadFromEntry(…, OscContextConfig&)` ~L2226)
- Modify: `src/contour/config/ConfigDocumentation.hpp` (`CommandBlocksConfig` after `OscContextConfig` ~L423; `CommandBlocksWeb` after `FoldingWeb` ~L2141; `using CommandBlocks` after `using OscContext` ~L2659)
- Test: `src/contour/config/Config_test.cpp` (after `Config: an unknown osc_context enum value keeps the default`, ~L1113)

**Interfaces:**
- Consumes: `vtbackend::CommandBlockStoreLimits`, `vtbackend::Settings::commandBlockLimits` (Task 1.8).
- Produces (C9): `struct CommandBlocksConfig { size_t maxRecords { 1000 }; };`, `ConfigEntry<CommandBlocksConfig, documentation::CommandBlocks> Config::commandBlocks`, global YAML key `command_blocks.max_records` (clamped to `[1, MaxCommandBlockRecords]`), `constexpr inline int64_t MaxCommandBlockRecords = 1'000'000;` (addition). Phase 5 adds `pager`.

- [ ] **Step 1: Write the failing tests** — in `src/contour/config/Config_test.cpp`, after the closing `}` of `Config: an unknown osc_context enum value keeps the default` insert:

```cpp
TEST_CASE("Config: command_blocks loads from YAML and reaches the emulation settings", "[config]")
{
    QTemporaryDir dir;
    // GLOBAL, like folding: it says how the terminal reads the protocol a shell speaks.
    auto const config = loadFromYaml(dir, R"(
default_profile: main
command_blocks:
    max_records: 250
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(config.commandBlocks.value().maxRecords == 250);

    auto const settings = contour::config::emulationSettings(config, *profile);
    CHECK(settings.commandBlockLimits.maxRecords == 250);
}

TEST_CASE("Config: command_blocks defaults to a thousand records and is written back", "[config]")
{
    auto const defaults = contour::config::Config {};
    CHECK(defaults.commandBlocks.value().maxRecords == 1000);

    auto const& profile = defaults.profiles.value().at(defaults.defaultProfileName.value());
    CHECK(contour::config::emulationSettings(defaults, profile).commandBlockLimits.maxRecords == 1000);

    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("command_blocks:"));
    CHECK(generated.contains("max_records: 1000"));
}

TEST_CASE("Config: command_blocks.max_records is clamped to a usable range", "[config]")
{
    auto const load = [](std::string_view value) {
        QTemporaryDir dir;
        auto const yaml = std::format("default_profile: main\n"
                                      "command_blocks:\n"
                                      "    max_records: {}\n"
                                      "profiles:\n"
                                      "    main:\n"
                                      "        shell: /bin/sh\n",
                                      value);
        return loadFromYaml(dir, yaml).commandBlocks.value().maxRecords;
    };

    // The current block must always resolve, so a store of none is a store of one...
    CHECK(load("0") == 1);
    // ...a negative count is not a reason to throw or wrap to "unbounded"...
    CHECK(load("-5") == 1);
    // ...and a flood of OSC 133 must not be able to cost unbounded memory through the configuration.
    CHECK(load("99999999999") == static_cast<size_t>(contour::config::MaxCommandBlockRecords));
}
```

(`Config_test.cpp` already includes `<format>`.)

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `error C2039: 'commandBlocks': is not a member of 'contour::config::Config'`. (If the Qt runtime is not staged, run the binary as the README describes; on the pre-existing `display/*` build break, build `Config_test.cpp`'s object alone as in Task 1.7 Step 9.)

- [ ] **Step 3: Declare the configuration** — in `src/contour/config/Config.hpp`, after the closing `};` of `struct OscContextConfig` insert:

```cpp

/// What the terminal remembers about each shell command block it saw run: command line, exit status,
/// duration, start time and working directory, kept even after the block's rows have scrolled away.
///
/// GLOBAL rather than per-profile, for the reason FoldingConfig is: it describes how the terminal reads
/// the protocol a shell speaks (OSC 133, OSC 3008), not how one pane looks.
struct CommandBlocksConfig
{
    /// How many blocks one session remembers; the oldest are forgotten first. Bounds what a flood of OSC
    /// 133 sequences -- a `cat` of a recorded session -- can cost. Applies to sessions opened afterwards.
    size_t maxRecords { 1000 };
};

/// The largest `command_blocks.max_records` the reader accepts; a larger value is clamped to it.
constexpr inline int64_t MaxCommandBlockRecords = 1'000'000;
```

after `ConfigEntry<OscContextConfig, documentation::OscContext> oscContext {};` insert:

```cpp
    ConfigEntry<CommandBlocksConfig, documentation::CommandBlocks> commandBlocks {};
```

after `void loadFromEntry(YAML::Node const& node, std::string const& entry, OscContextConfig& where);` insert:

```cpp
    void loadFromEntry(YAML::Node const& node, std::string const& entry, CommandBlocksConfig& where);
```

and after the `Writer` overload `format(std::string_view doc, OscContextConfig const& v)` insert:

```cpp

    [[nodiscard]] std::string format(std::string_view doc, CommandBlocksConfig const& v)
    {
        return format(doc, v.maxRecords);
    }
```

- [ ] **Step 4: Document it** — in `src/contour/config/ConfigDocumentation.hpp`, after the closing `};` of `constexpr StringLiteral OscContextConfig { … }` insert:

```cpp

constexpr StringLiteral CommandBlocksConfig {

    "command_blocks:\n"
    "    {comment} How many shell commands one session remembers: the command line, its exit\n"
    "    {comment} status, how long it ran, where and when -- even once its output has scrolled\n"
    "    {comment} away. The oldest are forgotten first. Needs a shell that emits OSC 133 (or\n"
    "    {comment} systemd's OSC 3008); without one nothing is recorded. At least 1, at most 1000000.\n"
    "    max_records: {}\n"
    "\n"

};
```

after the closing `};` of `constexpr StringLiteral FoldingWeb { … }` insert:

```cpp

constexpr StringLiteral CommandBlocksWeb {
    "configuration controls what the terminal remembers about each shell command it saw run.\n"
    "``` yaml\n"
    "command_blocks:\n"
    "  max_records: 1000\n"
    "```\n"
    "A command block is one prompt and the output of the command run at it, delimited by the OSC 133 "
    "marks a shell with shell integration emits -- or, without one, by systemd's OSC 3008 contexts. For "
    "each block the terminal keeps its command line, exit status, duration, start time and working "
    "directory, even after the block's rows have left the scrollback.\n"
    ":octicons-horizontal-rule-16: ==max_records== How many blocks one session remembers; the oldest are "
    "forgotten first. At least 1, at most 1000000. A change applies to sessions opened afterwards. <br/>\n"
};
```

and after `using OscContext = DocumentationEntry<OscContextConfig, OscContextWeb>;` insert:

```cpp
using CommandBlocks = DocumentationEntry<CommandBlocksConfig, CommandBlocksWeb>;
```

- [ ] **Step 5: Load and map it** — in `src/contour/config/Config.cpp`, after `loadFromEntry("osc_context", c.oscContext);` insert `loadFromEntry("command_blocks", c.commandBlocks);`; after the closing `}` of `YAMLConfigReader::loadFromEntry(…, OscContextConfig& where)` insert:

```cpp

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     CommandBlocksConfig& where)
{
    auto const child = node[entry];
    if (!child)
        return;

    // Read as a signed number and clamped, rather than parsed straight into size_t: a negative value would
    // otherwise make yaml-cpp throw, or wrap around to an effectively unbounded store.
    auto requested = static_cast<int64_t>(where.maxRecords);
    loadFromEntry(child, "max_records", requested);
    where.maxRecords = static_cast<size_t>(std::clamp<int64_t>(requested, 1, MaxCommandBlockRecords));
}
```

and in `emulationSettings`, after `settings.foldJumpBehavior = folding.onJumpIntoFold;` insert:

```cpp
    settings.commandBlockLimits =
        vtbackend::CommandBlockStoreLimits { .maxRecords = config.commandBlocks.value().maxRecords };
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test contour_test` then `out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
Expected: zero warnings; `All tests passed` — including the existing `Config: the generated default config loads back into the defaults`, whose empty error log proves the reader understands the new key the writer emits.

Hand-audit for clang-tidy (`src/contour/**` cannot be linted on this machine; CI is the oracle): `misc-const-correctness` (`requested` is mutated by `loadFromEntry`, so it stays non-const), `bugprone-narrowing-conversions` (both casts explicit), `readability-identifier-naming` (`MaxCommandBlockRecords` is a constexpr variable → CamelCase), `misc-use-internal-linkage` (nothing new at namespace scope in a `.cpp`).

- [ ] **Step 7: Format and commit**

Run: `clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp`

```bash
git add src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp
git commit -F - <<'EOF'
config: add command_blocks.max_records

A global key, like folding, mapped into vtbackend::Settings and from there
into each new terminal's CommandBlockStore. Clamped to 1..1000000; written by
`contour generate config` and documented for the configuration reference.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 1.14: Phase gate

**Files:** whatever `/simplify` and the review change; nothing else.

- [ ] **Step 1: Build everything, warning-free**

Run: `cmake --build --preset clangcl-debug`
Expected: success with zero warnings. On this machine's pre-existing `src/contour/display/*` `yaml-cpp/emitter.h` failure, build the test targets instead and record the break as pre-existing:
`cmake --build --preset clangcl-debug --target vtbackend_test vtparser_test vtrasterizer_test vthost_test vtworkspace_test vtconformance_test contour_test contour_gui_test`

- [ ] **Step 2: Run the phase's tests, then the suite**

Run, in order:
- `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"` — every phase-1 case.
- `out/build/clangcl-debug/bin/vtbackend_test.exe "SemanticBlockProtocol.*"` and `"ShellIntegration.*"` — the migrated and preserved protocol cases.
- `out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
- `ctest --test-dir out/build/clangcl-debug --output-on-failure`
- `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`

Expected: every test green except the failures recorded in the phase-0 baseline (e.g. `vtconformance_vttest` without `vttest -c`); spelling PASS (or SKIP when `typos` is absent, as at phase 0). Note the `N tests passed, M failed` line.

- [ ] **Step 3: Review Focus self-check** — confirm, by name, the three tests the README assigns to this phase are present and green:
  1. #1 `Terminal.blocks.idsTimesAndHeadsSurviveAColumnResizeMidCommand` (+ `aHeadEvictedBeforeTheResizeIsRecordedAsEvicted`, `Grid.reflow.blockIdAndBirthTimeRideOnEveryChunk`);
  3. #3 `CommandBlockStore.aHugeCommandLineIsStoredTruncatedAtACodepoint` and `Terminal.blocks.aHugeCmdlineUrlIsStoredTruncatedAtACodepoint`;
  4. #4 `CommandBlockStore.aFloodOfCyclesStaysWithinTheCap` (100k cycles) and `Terminal.blocks.aFloodOfPromptsStaysWithinTheCap`.

- [ ] **Step 4: `/simplify`** over `git diff $PHASE1_START..HEAD` (the hash noted in "Before you start"). Likely candidates it should weigh: the three `positionOf`/`newest*In` helpers; the two `logicalLineHead(cursor)` + `stableLineIdOf` computations in `beginCommandBlock`/`beginCommand` (one private helper `currentHead()` returning both); the repeated `iota(addressableTop, pageLines)` walk in `syncCommandBlockHeads` and `commandBlockAt`. Rebuild, rerun Step 2, then commit its fixes:

```bash
git add -u
git commit -F - <<'EOF'
vtbackend: simplify the command block foundation

<one line per change /simplify made>

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(No commit when it changed nothing.)

- [ ] **Step 5: Code review at xhigh** — `/code-review xhigh` on `$PHASE1_START..HEAD` (or a review subagent dispatched with `effort: "xhigh"`). Point the reviewer at: the `Line` tail layout and the `static_assert`; the reflow carry rule (split chunks vs rebuilt continuations); `Screen::setActiveBlockId` stamping the whole logical line; the parser-thread contract of `Events::commandBlockFinished` and `Events::localIdentity`; `syncCommandBlockHeads` mutating a `mutable` store from `const` readers; the mode 2034 visibility floor; id wrap-around in `CommandBlockStore::mint`; `hardReset` clearing the store before the pages re-seat their cursors; and the "Decisions made here" list at the top of this file. Fix every confirmed finding test-first, rebuild, rerun Step 2, and commit:

```bash
git add -u
git commit -F - <<'EOF'
vtbackend: address the phase 1 review

<one line per finding fixed>

ctest: <N> passed, <M> failed (<names of the baseline failures>)

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

The last commit of the phase carries the `ctest` summary line in its body. If neither Step 4 nor Step 5 changed anything, make no empty commit — report the summary line to the coordinator instead (phase 0's rule).

- [ ] **Step 6: Report** to the coordinating session: the ctest summary, the commits of this phase (`git log --oneline $PHASE1_START..HEAD`), any finding left open, and the contract notes at the top of this file. The coordinator records progress and propagates the contract notes into the README.

## Notes for later phases

- **Phase 2 (daemon):** `WireLine` must carry `blockId` and `bornAt`. `Line::stampBornAt()` only stamps an UNSTAMPED line, and `Line::adoptBlock()` ignores zero — a mirror applying a delta needs a raw setter (add `void Line::setBornAt(uint32_t) noexcept` there) so a row's stamp can be corrected rather than only first-set. The client terminal never samples a batch clock for mirrored rows unless it parses; keep it that way. `vthost::SessionHost`'s `Events` inherits the empty `localIdentity()`, so a daemon-hosted block's working-directory locality is computed without this machine's identity — give `SessionHost::Events` the daemon's identity if locality matters for a client. `LineFlag::UserMark` is already pinned on the wire (bit 9).
- **Phase 4 (gutter):** `outcomeOf()`, `Terminal::commandBlockAt()`, `Terminal::lineBirthTime()` are ready; a `Line::MaxBornAt` stamp reads as "no time".
- **Phases 7/8:** read heads only through `Terminal::commandBlocks()` (it re-records them after a reflow); a head below `Grid::stableRangeFloor()` means "evicted".
- **Phase 6 (notifications):** `vtbackend::truncateUtf8(text, maxBytes)` (`shell/CommandBlock.hpp`) is the one UTF-8 truncation helper; the ellipsis counts against `maxBytes`. Consume it — do not write another.

