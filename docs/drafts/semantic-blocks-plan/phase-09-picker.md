# Phase 9 — Recent-commands picker

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A picker over every open session's finished shell commands, opened by `OpenRecentCommands` (`Ctrl+Alt+R`, and a row in the action palette), that inserts the picked command at the focused session's prompt through the paste path (Enter) or inserts and runs it (Shift+Enter) — and refuses, saying why, when that session is not at a shell prompt.

**Architecture:** Every decision is a pure, Qt-free function in `contour/command/RecentCommands.hpp` (ranking, the per-session snapshot, prompt readiness, ages, the notice, the bytes to send), tested in `contour_test`. `TerminalSessionManager` snapshots each session's `CommandBlockStore` under that terminal's lock; `WindowController` owns a `RecentCommandsModel` (Qt list model over the snapshot) and pins the target session; `TerminalSession::insertRecentCommand` re-checks the prompt and writes through `Terminal::sendPaste`. The QML palette becomes one popup shell with a row-source seam: a `modes` table (row source, delegate, key role, on-accept callback) with one row for actions and one for recent commands; `Main.qml` declares one instance of each.

**Tech Stack:** C++23, Qt 6 (QAbstractListModel, QML, `Qt6::Test`), Catch2, CMake presets.

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §9, §10.4, §13.2 and the `Ctrl+Alt+R` item of §16.1. Global constraints, build commands, the Interface Contract (C1, C6, C8) and the Review Focus list: [README](README.md#global-constraints).

**Depends on:** phase 1 (C1: `CommandBlockStore`, `CommandBlockRecord`, `CommandBlockState::Prompting`, `Terminal::commandBlocks()`), phase 2 (mirrored stores — attached sessions take part with no code here), phase 3 (C6: `sanitizeCommandLine`), phase 5 (Task 5.17's `builtinFallbackCharMappings()`, which Task 9.10 adds the `Ctrl+Alt+R` row to).

---

## Decisions this phase makes (read before Task 9.1)

1. **Ranking** (`rankRecentCommands`, C8). Rows collapse by their RAW command line. The run a collapsed row shows is the newest run *in the current session* if the command ran there at all, else the newest run anywhere; `count` covers every run. Order: current-session tier first; then, for a non-empty query, best `fuzzyMatch` score first; then newest finish time; then later input row (each session's rows arrive oldest first). The query is matched against `sanitizeCommandLine(line, SanitizePurpose::Display)` — what the row displays — so `matchPositions` are byte offsets into that display text. An empty query keeps everything.
2. **Row-source seam.** `CommandPalette.qml` stays the only popup. It gains `enum Mode { Actions, RecentCommands }`, `enum Accept { Insert, Run }` (mirroring `command::RecentCommandAccept`) and a `modes` table whose row per mode names the controller property the rows come from, the delegate `Component`, the delegate property that keys the accepted row, the filter placeholder, whether the source has a `notice`, and the on-accept callback. The action row is the old delegate moved verbatim into a `Component`; `acceptCurrent()` keeps its zero-argument signature (tests invoke it by name). Action mode behaves exactly as before and its tests are not edited.
3. **Target pinning.** `WindowController::openRecentCommands(target)` pins the session in a `QPointer`, like `_contextMenuSession`: accepting inserts into the session the picker opened over, or does nothing if it died.
4. **Guard.** `PromptReadiness::AtPrompt` iff the store's `current()` is `Prompting` **and** the terminal shows the primary screen. Checked at open (for the notice) and again at accept, under the same lock hold as the paste.
5. **Notice.** `RecentCommandsNotice::NoCommands` (no session has a finished command — README Review Focus #5) outranks `NotAtPrompt`. The empty-state hint names the one CLI verb that installs an integration, `contour generate integration shell SHELL to FILE` (`src/contour/cli/ContourApp.cpp:1081-1095`); the spec's shorter wording names no verb that exists (see the phase report).
6. **Palette entry.** Adding `OpenRecentCommands` to the action catalog is what puts it in the action palette; its title is derived like every other ("Open Recent Commands", `CommandCatalog.cpp:30-33`). No authored title table is introduced for one row.
7. **Chord.** `Ctrl+Alt+R` is free: `defaultInputMappings()` (`Config.hpp:794-1225`) has no platform branches and binds no `R`, `builtinFallbackKeyMappings()` (`Config.cpp:108-146`) binds only PageUp/PageDown/Tab, and `builtinFallbackCharMappings()` (Task 5.17) binds only `Ctrl+Shift+G`. It is bound as a row of `builtinFallbackCharMappings()`, not as a default, for phase 5 Decision 6's reason: a new default never reaches a user whose generated contour.yml already lists `input_mapping:` (`Config.hpp:172-178`). Task 9.10 re-verifies the chord with a grep before binding, and a test pins that the fallback binds it and no default claims it first.

**Additions to the contract (defined here, used only by this phase):** in `contour/command/RecentCommands.hpp` — `PromptReadiness`, `recentCommandRowsOf`, `promptReadinessOf`, `RecentCommandStatus`, `statusOf`, `formatAge`, `RecentCommandsSnapshot`, `RecentCommandsNotice`, `recentCommandsNotice`, `RecentCommandAccept`, `RecentCommandError`, `RecentCommandInput`, `recentCommandInput`; in `FuzzyFilter.hpp` — `utf16Indices`; `window::RecentCommandsModel`; `TerminalSession::promptReadiness`/`insertRecentCommand`; `TerminalSessionManager::openRecentCommands`/`recentCommandsSnapshot`; `WindowController::recentCommands`/`openRecentCommands`/`acceptRecentCommand`/`recentCommandsRequested`. Exact signatures are in each task's **Interfaces** block.

**clang-tidy hand audit (src/contour/** cannot be linted on Windows, README):** every helper in a `.cpp` sits in an anonymous namespace (`misc-use-internal-linkage`); every local that is not mutated is `const` (`misc-const-correctness`); no `int * int` result is widened to `size_t` (`bugprone-implicit-widening-of-multiplication-result`). No identifier in this phase is a Windows SDK macro name (`small`, `near`, `far`, `min`, `max`, `interface`, `boolean`, `hyper`, `byte`).

---

### Task 9.1: `rankRecentCommands` — the picker's order

**Files:**
- Create: `src/contour/command/RecentCommands.hpp`, `src/contour/command/RecentCommands.cpp`, `src/contour/command/RecentCommands_test.cpp`
- Modify: `src/contour/command/CMakeLists.txt:9-19` (sources), `src/contour/CMakeLists.txt:805-821` (`contour_test` sources)

**Interfaces:**
- Consumes: `command::fuzzyMatch` (`FuzzyFilter.hpp:73-75`); C6 `vtbackend::sanitizeCommandLine(std::string_view, SanitizePurpose)` (`src/vtbackend/shell/CommandLineSanitizer.hpp`).
- Produces (C8, exactly):
  ```cpp
  struct RecentCommandRow { std::string commandLine; std::optional<int> exitCode; std::string workingDirectory;
                            std::chrono::system_clock::time_point finishedAt {}; uint64_t sessionId {}; std::string tabName; };
  struct RankedRecentCommand { RecentCommandRow newest; size_t count = 1; std::vector<size_t> matchPositions; };
  [[nodiscard]] std::vector<RankedRecentCommand> rankRecentCommands(std::span<RecentCommandRow const> rows,
                                                                    uint64_t currentSessionId, std::string_view query);
  ```

`contour_command` is Qt-free by construction: it links only `contour_config core::base` (`src/contour/command/CMakeLists.txt:21`), and `contour_config` links no Qt (`src/contour/config/CMakeLists.txt:1-36`). `vtbackend` (and through `vtpty`, `core::platform`) reaches it via `contour_config`'s PUBLIC links, which is what lets this header name vtbackend types in Task 9.2.

- [ ] **Step 1: Write the failing tests**

Create `src/contour/command/RecentCommands_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The recent-commands picker's decisions (spec §9). What the ranking must get right is ORDER: the
// command the user is reaching for has to be under the cursor when the picker opens, and typing has to
// narrow the list without shuffling the sessions together.

#include <contour/command/RecentCommands.hpp>

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using contour::command::rankRecentCommands;
using contour::command::RankedRecentCommand;
using contour::command::RecentCommandRow;

namespace
{
constexpr auto Epoch = std::chrono::system_clock::time_point { std::chrono::seconds { 1'700'000'000 } };
constexpr auto Here = uint64_t { 1 };
constexpr auto There = uint64_t { 2 };
constexpr auto Elsewhere = uint64_t { 3 };

/// A command of session @p sessionId that finished @p at after the test epoch.
[[nodiscard]] RecentCommandRow ran(std::string commandLine,
                                   uint64_t sessionId,
                                   std::chrono::seconds at,
                                   std::optional<int> exitCode = 0)
{
    return RecentCommandRow { .commandLine = std::move(commandLine),
                              .exitCode = exitCode,
                              .workingDirectory = "/home/user",
                              .finishedAt = Epoch + at,
                              .sessionId = sessionId,
                              .tabName = sessionId == Here ? "here" : "there" };
}

/// The command lines of @p ranked, in order.
[[nodiscard]] std::vector<std::string> commandsOf(std::vector<RankedRecentCommand> const& ranked)
{
    auto result = std::vector<std::string> {};
    for (auto const& entry: ranked)
        result.push_back(entry.newest.commandLine);
    return result;
}
} // namespace

TEST_CASE("rankRecentCommands: nothing in, nothing out", "[contour][picker]")
{
    CHECK(rankRecentCommands({}, Here, "").empty());
    CHECK(rankRecentCommands({}, Here, "make").empty());
}

TEST_CASE("rankRecentCommands: the current session's commands come first, newest first", "[contour][picker]")
{
    auto const rows = std::vector {
        ran("git status", Here, 10s),
        ran("make", There, 50s),
        ran("ls", Here, 30s),
        ran("cargo test", Elsewhere, 40s),
        ran("vim notes.md", There, 20s),
    };

    CHECK(commandsOf(rankRecentCommands(rows, Here, ""))
          == std::vector<std::string> { "ls", "git status", "make", "cargo test", "vim notes.md" });
}

TEST_CASE("rankRecentCommands: with no current session everything ranks by recency", "[contour][picker]")
{
    auto const rows = std::vector { ran("a", Here, 10s), ran("b", There, 30s), ran("c", Elsewhere, 20s) };

    CHECK(commandsOf(rankRecentCommands(rows, 0, "")) == std::vector<std::string> { "b", "c", "a" });
}

TEST_CASE("rankRecentCommands: identical command lines collapse into one row", "[contour][picker]")
{
    SECTION("with a count, showing the newest run")
    {
        auto const rows =
            std::vector { ran("make", There, 10s, 2), ran("make", There, 30s, 0), ran("ls", There, 20s) };
        auto const ranked = rankRecentCommands(rows, Here, "");

        REQUIRE(ranked.size() == 2);
        CHECK(ranked[0].newest.commandLine == "make");
        CHECK(ranked[0].count == 2);
        CHECK(ranked[0].newest.finishedAt == Epoch + 30s);
        CHECK(ranked[0].newest.exitCode == 0);
        CHECK(ranked[1].count == 1);
    }

    SECTION("a command also run here ranks with this session, as of its run here")
    {
        // The newer run is in another tab, but the user ran it HERE too: the row belongs to this
        // session's tier and reports how it went here, while the count still covers every run.
        auto const rows =
            std::vector { ran("make", Here, 10s, 2), ran("ls", Here, 20s), ran("make", There, 90s, 0) };
        auto const ranked = rankRecentCommands(rows, Here, "");

        CHECK(commandsOf(ranked) == std::vector<std::string> { "ls", "make" });
        REQUIRE(ranked.size() == 2);
        CHECK(ranked[1].count == 2);
        CHECK(ranked[1].newest.sessionId == Here);
        CHECK(ranked[1].newest.exitCode == 2);
        CHECK(ranked[1].newest.finishedAt == Epoch + 10s);
    }

    SECTION("lines that only LOOK alike once sanitised stay apart")
    {
        auto const rows = std::vector { ran("ls\x01", Here, 10s), ran("ls\x02", Here, 20s) };
        CHECK(rankRecentCommands(rows, Here, "").size() == 2);
    }
}

TEST_CASE("rankRecentCommands: a tie in finish time goes to the later record", "[contour][picker]")
{
    // Each session's rows arrive oldest first, so of two commands that finished in the same instant the
    // one listed later is the newer record.
    SECTION("between two rows")
    {
        auto const rows = std::vector { ran("first", Here, 10s), ran("second", Here, 10s) };
        CHECK(commandsOf(rankRecentCommands(rows, Here, "")) == std::vector<std::string> { "second", "first" });
    }

    SECTION("between two runs of one command")
    {
        auto const rows = std::vector { ran("make", There, 10s, 1), ran("make", There, 10s, 0) };
        auto const ranked = rankRecentCommands(rows, Here, "");
        REQUIRE(ranked.size() == 1);
        CHECK(ranked[0].newest.exitCode == 0);
    }
}

TEST_CASE("rankRecentCommands: a query filters with the palette's fuzzy matcher", "[contour][picker]")
{
    auto const rows = std::vector {
        ran("git status", Here, 10s),
        ran("grep -r foo", Here, 20s),
        ran("git push", There, 30s),
    };

    SECTION("only matching commands survive, this session's still first")
    {
        CHECK(commandsOf(rankRecentCommands(rows, Here, "git"))
              == std::vector<std::string> { "git status", "git push" });
    }

    SECTION("matching ignores case")
    {
        CHECK(commandsOf(rankRecentCommands(rows, Here, "GIT"))
              == std::vector<std::string> { "git status", "git push" });
    }

    SECTION("the matched characters are reported for highlighting")
    {
        auto const ranked = rankRecentCommands(rows, Here, "gst");
        REQUIRE(commandsOf(ranked) == std::vector<std::string> { "git status" });
        CHECK(ranked[0].matchPositions == std::vector<size_t> { 0, 4, 5 });
    }

    SECTION("a query that matches nothing yields nothing")
    {
        CHECK(rankRecentCommands(rows, Here, "zzz").empty());
    }

    SECTION("an empty query highlights nothing")
    {
        for (auto const& entry: rankRecentCommands(rows, Here, ""))
            CHECK(entry.matchPositions.empty());
    }
}

TEST_CASE("rankRecentCommands: within a session the better match leads, across sessions this one does",
          "[contour][picker]")
{
    // "mk" is a contiguous word-start hit in "mkdir -p tmp/x" and "mkfs" (score 30) but a gapped one in
    // "make test" (score 14). The score reorders within this session's tier; it never lifts another
    // session's command above this one's.
    auto const rows = std::vector {
        ran("mkdir -p tmp/x", Here, 10s),
        ran("make test", Here, 50s),
        ran("mkfs", There, 90s),
    };

    CHECK(commandsOf(rankRecentCommands(rows, Here, "mk"))
          == std::vector<std::string> { "mkdir -p tmp/x", "make test", "mkfs" });
}

TEST_CASE("rankRecentCommands: match positions index the DISPLAYED command line", "[contour][picker]")
{
    // A control character is shown as a placeholder of a different byte length, so positions measured in
    // the raw line would highlight the wrong characters.
    auto const raw = std::string { "echo \x1b[31mred" };
    auto const rows = std::vector { ran(raw, Here, 10s) };
    auto const ranked = rankRecentCommands(rows, Here, "red");
    REQUIRE(ranked.size() == 1);

    auto const display = vtbackend::sanitizeCommandLine(raw, vtbackend::SanitizePurpose::Display);
    auto const at = display.rfind("red");
    REQUIRE(at != std::string::npos);
    CHECK(ranked[0].matchPositions == std::vector<size_t> { at, at + 1, at + 2 });
}
```

Register it: in `src/contour/CMakeLists.txt`, in the `add_executable(contour_test ...)` list, add the line after `command/FuzzyFilter_test.cpp`:

```cmake
        command/RecentCommands_test.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `cannot open include file 'contour/command/RecentCommands.hpp'`.

- [ ] **Step 3: Write the header**

Create `src/contour/command/RecentCommands.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour::command
{

/// One finished shell command, as the recent-commands picker sees it (spec §9).
///
/// A plain value copied out of a session's CommandBlockStore under that terminal's lock, so the picker
/// never reads terminal state again after it opened.
struct RecentCommandRow
{
    std::string commandLine;     ///< RAW, as reported or recovered; sanitise per use (spec §10.4).
    std::optional<int> exitCode; ///< The reported exit code; nullopt when none is known.
    std::string workingDirectory; ///< Where the command ran (the store's snapshot at OSC 133;C).
    std::chrono::system_clock::time_point finishedAt {}; ///< When it finished, by the wall clock.
    uint64_t sessionId {};                               ///< The model id of the session that ran it.
    std::string tabName;                                 ///< The label of the tab hosting that session.
};

/// One row of the picker: every run of one command line, collapsed.
struct RankedRecentCommand
{
    RecentCommandRow newest; ///< The run the row shows; see rankRecentCommands() for which one.
    size_t count = 1;        ///< How many runs collapsed into the row.
    /// Byte offsets into sanitizeCommandLine(newest.commandLine, SanitizePurpose::Display) that the query
    /// matched, ascending; empty for an empty query.
    std::vector<size_t> matchPositions;
};

/// Ranks every open session's recent commands for the picker (spec §9).
///
/// - Identical command lines (compared RAW) collapse into one entry carrying the number of runs. The run
///   it shows is the newest one in @p currentSessionId if the command ran there at all, else the newest
///   one anywhere, so a row reports how the command went where the user is about to run it again.
/// - Entries whose shown run is in @p currentSessionId come first, every other entry after them.
/// - Within each of the two, a non-empty @p query keeps only the entries it fuzzy-matches (fuzzyMatch(),
///   case-insensitive, against the DISPLAYED command line) and puts the best match first. Recency breaks
///   ties, newest first; on equal finish times the later element of @p rows wins, because each session's
///   rows arrive oldest first. An empty query keeps everything and orders by recency alone.
///
/// @param rows             Every session's rows, each session's oldest first (recentCommandRowsOf()).
/// @param currentSessionId The session the picker would insert into; 0 when there is none.
/// @param query            What the user typed; empty for the unfiltered list.
/// @return The picker's rows, in display order.
[[nodiscard]] std::vector<RankedRecentCommand> rankRecentCommands(std::span<RecentCommandRow const> rows,
                                                                  uint64_t currentSessionId,
                                                                  std::string_view query);

} // namespace contour::command
```

- [ ] **Step 4: Write the implementation**

Create `src/contour/command/RecentCommands.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <contour/command/FuzzyFilter.hpp>
#include <contour/command/RecentCommands.hpp>

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace contour::command
{

namespace
{
    /// Every run of one command line: the run the picker row shows, and how many there were.
    struct Group
    {
        size_t representative; ///< Index into the rows of the run the row shows.
        size_t count;          ///< How many runs share the command line.
    };

    /// Which half of the list an entry belongs to; the current session's half comes first.
    enum class Tier : uint8_t
    {
        ThisSession = 0,
        OtherSessions,
    };

    /// One group that survived the query, with everything the ordering compares.
    struct Candidate
    {
        Group group;
        Tier tier;
        int score;
        std::vector<size_t> positions;
    };

    /// Whether the run at @p candidate should represent its group instead of the run at @p incumbent.
    ///
    /// A run in the current session beats one elsewhere, so the row reports how the command went HERE;
    /// among runs of equal standing the later one wins, by finish time and then by input order.
    [[nodiscard]] bool supersedes(std::span<RecentCommandRow const> rows,
                                  size_t candidate,
                                  size_t incumbent,
                                  uint64_t currentSessionId) noexcept
    {
        auto const& challenger = rows[candidate];
        auto const& holder = rows[incumbent];
        auto const challengerHere = challenger.sessionId == currentSessionId;
        auto const holderHere = holder.sessionId == currentSessionId;
        if (challengerHere != holderHere)
            return challengerHere;
        if (challenger.finishedAt != holder.finishedAt)
            return challenger.finishedAt > holder.finishedAt;
        return candidate > incumbent;
    }

    /// Collapses identical command lines. Keyed by the RAW line: two lines that merely look alike once
    /// sanitised (they differ in a control byte) are different commands and stay apart.
    [[nodiscard]] std::vector<Group> groupRuns(std::span<RecentCommandRow const> rows, uint64_t currentSessionId)
    {
        auto groups = std::vector<Group> {};
        auto groupOf = std::unordered_map<std::string_view, size_t> {};
        for (auto const index: std::views::iota(size_t { 0 }, rows.size()))
        {
            auto const [slot, inserted] = groupOf.try_emplace(rows[index].commandLine, groups.size());
            if (inserted)
            {
                groups.push_back(Group { .representative = index, .count = 1 });
                continue;
            }
            auto& group = groups[slot->second];
            ++group.count;
            if (supersedes(rows, index, group.representative, currentSessionId))
                group.representative = index;
        }
        return groups;
    }

    /// The picker's order: this session's tier first; then the better match; then the newer run; then
    /// the later record. A strict total order, because no two groups share a representative.
    [[nodiscard]] bool ranksBefore(std::span<RecentCommandRow const> rows,
                                   Candidate const& a,
                                   Candidate const& b) noexcept
    {
        if (a.tier != b.tier)
            return a.tier < b.tier;
        if (a.score != b.score)
            return a.score > b.score;
        auto const& newestA = rows[a.group.representative];
        auto const& newestB = rows[b.group.representative];
        if (newestA.finishedAt != newestB.finishedAt)
            return newestA.finishedAt > newestB.finishedAt;
        return a.group.representative > b.group.representative;
    }
} // namespace

std::vector<RankedRecentCommand> rankRecentCommands(std::span<RecentCommandRow const> rows,
                                                    uint64_t currentSessionId,
                                                    std::string_view query)
{
    auto candidates = std::vector<Candidate> {};
    for (auto const& group: groupRuns(rows, currentSessionId))
    {
        auto const& newest = rows[group.representative];

        // Matched against what the row DISPLAYS, so the positions are positions in the text the user
        // sees: a control byte is shown as a placeholder of a different length.
        auto const display =
            vtbackend::sanitizeCommandLine(newest.commandLine, vtbackend::SanitizePurpose::Display);
        auto match = fuzzyMatch(query, display);
        if (!match)
            continue;

        auto positions = std::vector<size_t> {};
        positions.reserve(match->positions.size());
        for (auto const position: match->positions)
            positions.push_back(static_cast<size_t>(position));

        candidates.push_back(
            Candidate { .group = group,
                        .tier = newest.sessionId == currentSessionId ? Tier::ThisSession : Tier::OtherSessions,
                        .score = match->score,
                        .positions = std::move(positions) });
    }

    std::ranges::sort(candidates,
                      [rows](Candidate const& a, Candidate const& b) { return ranksBefore(rows, a, b); });

    auto ranked = std::vector<RankedRecentCommand> {};
    ranked.reserve(candidates.size());
    for (auto& candidate: candidates)
        ranked.push_back(RankedRecentCommand { .newest = rows[candidate.group.representative],
                                               .count = candidate.group.count,
                                               .matchPositions = std::move(candidate.positions) });
    return ranked;
}

} // namespace contour::command
```

Register the sources: in `src/contour/command/CMakeLists.txt`, inside `add_library(contour_command STATIC ...)`, add after `FuzzyFilter.cpp FuzzyFilter.hpp`:

```cmake
    RecentCommands.cpp RecentCommands.hpp
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test && out/build/clangcl-debug/bin/contour_test.exe "[picker]"`
Expected: `All tests passed` (8 test cases). Then `clang-format -i src/contour/command/RecentCommands.hpp src/contour/command/RecentCommands.cpp src/contour/command/RecentCommands_test.cpp`.

- [ ] **Step 6: Commit**

```bash
git add src/contour/command/RecentCommands.hpp src/contour/command/RecentCommands.cpp \
        src/contour/command/RecentCommands_test.cpp src/contour/command/CMakeLists.txt src/contour/CMakeLists.txt
git commit -F - <<'EOF'
command: rank recent shell commands for the picker

rankRecentCommands() collapses identical command lines, lists the current
session's commands first and filters with the palette's fuzzy matcher,
reporting match positions in the displayed (sanitised) text.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.2: Rows and prompt readiness from a session's store

**Files:**
- Modify: `src/contour/command/RecentCommands.hpp`, `src/contour/command/RecentCommands.cpp`, `src/contour/command/RecentCommands_test.cpp`

**Interfaces:**
- Consumes: C1 `vtbackend::CommandBlockStore` (`forEachRecord` oldest first, `current()`), `CommandBlockRecord`, `CommandBlockState`, `CommandBlockEnd`, `CommandStart`, `CommandBlockStoreLimits`; `vtbackend::ContextLocality` (`src/vtbackend/core/TerminalContext.hpp:553`, via `WorkingDirectorySnapshot`); `vtbackend::ScreenType` (`src/vtbackend/core/Primitives.hpp:578-582`); `core::platform::ManualClock`/`ManualWallClock` (`vendor/core-cpp/src/core/platform/Clock.hpp:127,214`).
- Produces:
  ```cpp
  // RecentCommandRow (C8) gains a last member (README Phase 9 amendment):
  //   vtbackend::ContextLocality locality = vtbackend::ContextLocality::Unknown;
  enum class PromptReadiness : uint8_t { NotAtPrompt = 0, AtPrompt };
  [[nodiscard]] std::vector<RecentCommandRow> recentCommandRowsOf(vtbackend::CommandBlockStore const& store,
                                                                  uint64_t sessionId, std::string_view tabName);
  [[nodiscard]] PromptReadiness promptReadinessOf(vtbackend::CommandBlockStore const& store,
                                                  vtbackend::ScreenType screen) noexcept;
  ```

`finishedAt` is the record's `commandStartedAt` (falling back to `promptStartedAt`) plus its steady `duration` converted to the system clock's period — the store keeps no finish instant of its own (C1). An implicitly closed record (`CommandBlockEnd::Implicit`: no `;D`, so no duration) ended when the next prompt appeared, so its `finishedAt` is the next record's `promptStartedAt` — a long `ssh` that just ended shows its age from its end, not its start. `locality` copies the record's `workingDirectory.locality`, so the picker abbreviates exactly the paths phase 6 does (Task 9.6).

- [ ] **Step 1: Write the failing tests**

In `src/contour/command/RecentCommands_test.cpp`, add to the includes:

```cpp
#include <core/platform/Clock.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
```

and append to the end of the file:

```cpp
namespace
{
/// A command-block store on clocks the test drives, fed the way an integrated shell feeds it.
struct ShellSession
{
    core::platform::ManualClock steady {};
    core::platform::ManualWallClock wall { Epoch };
    vtbackend::CommandBlockStore store { vtbackend::CommandBlockStoreLimits {}, steady, wall };

    /// OSC 133;A — a prompt is shown.
    void prompt() { store.promptStarted(0, 0); }

    /// OSC 133;C — Enter was pressed on @p commandLine (nullopt: the shell did not say what it was).
    void start(std::optional<std::string> commandLine, std::string cwd = "/home/user/src")
    {
        auto const source =
            commandLine ? vtbackend::CommandLineSource::Reported : vtbackend::CommandLineSource::None;
        store.commandStarted(vtbackend::CommandStart {
            .commandLine = std::move(commandLine),
            .source = source,
            .workingDirectory = { .path = std::move(cwd), .locality = vtbackend::ContextLocality::Local },
            .headStableId = 0,
            .headIdGeneration = 0 });
    }

    /// Lets @p duration pass on both clocks, then OSC 133;D with @p exitCode.
    void finish(int exitCode, std::chrono::seconds duration)
    {
        steady.advance(duration);
        wall.advance(duration);
        [[maybe_unused]] auto const summary = store.commandFinished(exitCode);
    }
};
} // namespace

TEST_CASE("recentCommandRowsOf: finished commands with a command line, oldest first", "[contour][picker]")
{
    auto shell = ShellSession {};
    shell.prompt();
    shell.start("make", "/home/user/src/contour");
    shell.finish(2, 3s);
    shell.prompt();
    shell.start("ls -l");
    shell.finish(0, 1s);
    shell.prompt(); // waiting at the next prompt: not a command yet

    auto const rows = contour::command::recentCommandRowsOf(shell.store, 7, "build");

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].commandLine == "make");
    CHECK(rows[0].exitCode == 2);
    CHECK(rows[0].workingDirectory == "/home/user/src/contour");
    CHECK(rows[0].finishedAt == Epoch + 3s);
    CHECK(rows[0].sessionId == 7);
    CHECK(rows[0].tabName == "build");
    CHECK(rows[0].locality == vtbackend::ContextLocality::Local);
    CHECK(rows[1].commandLine == "ls -l");
    CHECK(rows[1].exitCode == 0);
    CHECK(rows[1].finishedAt == Epoch + 4s);
}

TEST_CASE("recentCommandRowsOf: what is left out", "[contour][picker]")
{
    auto shell = ShellSession {};

    SECTION("no shell integration at all: an empty store")
    {
        CHECK(contour::command::recentCommandRowsOf(shell.store, 1, "").empty());
    }

    SECTION("a command still running")
    {
        shell.prompt();
        shell.start("sleep 10");
        CHECK(contour::command::recentCommandRowsOf(shell.store, 1, "").empty());
    }

    SECTION("a command the shell never named")
    {
        shell.prompt();
        shell.start(std::nullopt);
        shell.finish(0, 1s);
        CHECK(contour::command::recentCommandRowsOf(shell.store, 1, "").empty());
    }
}

TEST_CASE("recentCommandRowsOf: a command closed by the next prompt is listed without an exit code",
          "[contour][picker]")
{
    // An `ssh` whose remote side never reported ;D is closed implicitly by the next local prompt. It is
    // still a command the user may want to run again; there is just no exit status to show.
    auto shell = ShellSession {};
    shell.prompt();
    shell.start("ssh build-host");
    shell.steady.advance(5s);
    shell.wall.advance(5s);
    shell.prompt();

    auto const rows = contour::command::recentCommandRowsOf(shell.store, 1, "");

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].commandLine == "ssh build-host");
    CHECK_FALSE(rows[0].exitCode.has_value());
    // It ended when the next prompt appeared, not when it started: its age counts from there.
    CHECK(rows[0].finishedAt == Epoch + 5s);
}

TEST_CASE("promptReadinessOf: only a prompt on the primary screen takes an insertion", "[contour][picker]")
{
    using contour::command::promptReadinessOf;
    using contour::command::PromptReadiness;
    using vtbackend::ScreenType;

    auto shell = ShellSession {};

    SECTION("no shell integration: no prompt is known")
    {
        CHECK(promptReadinessOf(shell.store, ScreenType::Primary) == PromptReadiness::NotAtPrompt);
    }

    SECTION("at the prompt")
    {
        shell.prompt();
        CHECK(promptReadinessOf(shell.store, ScreenType::Primary) == PromptReadiness::AtPrompt);
    }

    SECTION("while a command runs")
    {
        shell.prompt();
        shell.start("make");
        CHECK(promptReadinessOf(shell.store, ScreenType::Primary) == PromptReadiness::NotAtPrompt);
    }

    SECTION("on the alternate screen, even with the prompt record still open")
    {
        // A shell that never reports ;C leaves its block Prompting while vim runs.
        shell.prompt();
        CHECK(promptReadinessOf(shell.store, ScreenType::Alternate) == PromptReadiness::NotAtPrompt);
    }

    SECTION("between ;D and the next ;A")
    {
        shell.prompt();
        shell.start("make");
        shell.finish(0, 1s);
        CHECK(promptReadinessOf(shell.store, ScreenType::Primary) == PromptReadiness::NotAtPrompt);
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'recentCommandRowsOf': is not a member of 'contour::command'` (and the same for `promptReadinessOf`, `PromptReadiness`).

- [ ] **Step 3: Declare the functions**

In `src/contour/command/RecentCommands.hpp`, add to the includes (above `<chrono>`):

```cpp
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

```

In `struct RecentCommandRow`, directly after
`    std::string tabName;                                 ///< The label of the tab hosting that session.`, add:

```cpp
    /// Whether @ref workingDirectory names this machine. A Foreign path is shown as is, never abbreviated
    /// to `~`, as the finish notification shows it (Task 9.6).
    vtbackend::ContextLocality locality = vtbackend::ContextLocality::Unknown;
```

and insert before the closing `} // namespace contour::command`:

```cpp
/// Whether a session can take an inserted command right now.
enum class PromptReadiness : uint8_t
{
    NotAtPrompt = 0, ///< A command runs, a full-screen program owns the screen, or no prompt is known.
    AtPrompt,        ///< The shell waits at its prompt on the primary screen.
};

/// The rows one session contributes to the picker: its FINISHED records that carry a command line,
/// oldest first (the store's own order). A prompt still being edited, a command still running and a
/// block whose command line was neither reported nor recovered are left out: there is nothing in them to
/// run again. A block closed implicitly (by the next prompt, no ;D) finished when that prompt appeared,
/// and is stamped so.
///
/// Call it with the session's terminal locked; the result is a plain copy that outlives the lock.
/// @param store     The session's command-block store.
/// @param sessionId The session's model id, stamped on every row.
/// @param tabName   The label of the tab hosting the session, stamped on every row.
/// @return The rows, oldest first.
[[nodiscard]] std::vector<RecentCommandRow> recentCommandRowsOf(vtbackend::CommandBlockStore const& store,
                                                                uint64_t sessionId,
                                                                std::string_view tabName);

/// Whether the picker may insert into a session: only while its newest block is still at the prompt
/// (CommandBlockState::Prompting) AND the primary screen is showing. The second half matters on its own:
/// a shell whose integration never reports output start (no OSC 133;C) leaves its block Prompting while
/// vim or less runs on the alternate screen.
/// @param store  The session's command-block store (terminal locked).
/// @param screen The screen the terminal shows.
/// @return AtPrompt only when both hold.
[[nodiscard]] PromptReadiness promptReadinessOf(vtbackend::CommandBlockStore const& store,
                                                vtbackend::ScreenType screen) noexcept;
```

- [ ] **Step 4: Implement them**

In `src/contour/command/RecentCommands.cpp`, add `#include <chrono>` and `#include <optional>` to the standard includes and insert before the closing `} // namespace contour::command`:

```cpp
std::vector<RecentCommandRow> recentCommandRowsOf(vtbackend::CommandBlockStore const& store,
                                                  uint64_t sessionId,
                                                  std::string_view tabName)
{
    auto rows = std::vector<RecentCommandRow> {};

    // The row of an implicitly closed block, waiting for the record after it: that block ended when the
    // next prompt appeared, and only the next record knows when that was.
    auto awaitingNextPrompt = std::optional<size_t> {};

    store.forEachRecord([&](vtbackend::CommandBlockRecord const& record) {
        if (awaitingNextPrompt)
        {
            rows[*awaitingNextPrompt].finishedAt = record.promptStartedAt;
            awaitingNextPrompt.reset();
        }

        if (record.state != vtbackend::CommandBlockState::Finished || record.commandLine.empty())
            return;

        // The store keeps when a command started and how long it ran (steady clock), not when it
        // finished; the two together are the finish instant on the wall clock.
        auto const startedAt = record.commandStartedAt.value_or(record.promptStartedAt);
        auto const ranFor = std::chrono::duration_cast<std::chrono::system_clock::duration>(
            record.duration.value_or(std::chrono::steady_clock::duration::zero()));

        rows.push_back(RecentCommandRow { .commandLine = record.commandLine,
                                          .exitCode = record.exitCode,
                                          .workingDirectory = record.workingDirectory.path,
                                          .finishedAt = startedAt + ranFor,
                                          .sessionId = sessionId,
                                          .tabName = std::string { tabName },
                                          .locality = record.workingDirectory.locality });

        // No duration to add: its start stands in until the record after it says when the next prompt came.
        if (record.end == vtbackend::CommandBlockEnd::Implicit)
            awaitingNextPrompt = rows.size() - 1;
    });
    return rows;
}

PromptReadiness promptReadinessOf(vtbackend::CommandBlockStore const& store, vtbackend::ScreenType screen) noexcept
{
    if (screen != vtbackend::ScreenType::Primary)
        return PromptReadiness::NotAtPrompt;

    auto const* current = store.current();
    if (current == nullptr || current->state != vtbackend::CommandBlockState::Prompting)
        return PromptReadiness::NotAtPrompt;

    return PromptReadiness::AtPrompt;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test && out/build/clangcl-debug/bin/contour_test.exe "[picker]"`
Expected: `All tests passed` (12 test cases). `clang-format -i` the three files.

- [ ] **Step 6: Commit**

```bash
git add src/contour/command/RecentCommands.hpp src/contour/command/RecentCommands.cpp \
        src/contour/command/RecentCommands_test.cpp
git commit -F - <<'EOF'
command: read picker rows and prompt readiness from a command-block store

recentCommandRowsOf() copies a session's finished, named commands out of its
store, with the working directory's locality; a block the next prompt closed
is stamped as finishing when that prompt appeared. promptReadinessOf() admits
an insertion only at a Prompting block on the primary screen.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.3: What a row and the picker say — status, age, snapshot, notice

**Files:**
- Modify: `src/contour/command/RecentCommands.hpp`, `src/contour/command/RecentCommands.cpp`, `src/contour/command/RecentCommands_test.cpp`

**Interfaces:**
- Consumes: Task 9.1/9.2 types.
- Produces:
  ```cpp
  enum class RecentCommandStatus : uint8_t { Unknown = 0, Succeeded, Failed };
  [[nodiscard]] constexpr RecentCommandStatus statusOf(std::optional<int> exitCode) noexcept;
  [[nodiscard]] std::string formatAge(std::chrono::system_clock::duration age);
  struct RecentCommandsSnapshot { std::vector<RecentCommandRow> rows; uint64_t currentSessionId {};
                                  PromptReadiness readiness = PromptReadiness::NotAtPrompt; };
  enum class RecentCommandsNotice : uint8_t { None = 0, NoCommands, NotAtPrompt };
  [[nodiscard]] RecentCommandsNotice recentCommandsNotice(RecentCommandsSnapshot const& snapshot) noexcept;
  ```

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/command/RecentCommands_test.cpp`:

```cpp
TEST_CASE("statusOf: the exit code's mark", "[contour][picker]")
{
    using contour::command::RecentCommandStatus;
    using contour::command::statusOf;

    CHECK(statusOf(std::nullopt) == RecentCommandStatus::Unknown);
    CHECK(statusOf(0) == RecentCommandStatus::Succeeded);
    CHECK(statusOf(2) == RecentCommandStatus::Failed);
    CHECK(statusOf(-1) == RecentCommandStatus::Failed);
}

TEST_CASE("formatAge: the largest whole unit, rounded down", "[contour][picker]")
{
    using contour::command::formatAge;

    CHECK(formatAge(0s) == "0s ago");
    CHECK(formatAge(500ms) == "0s ago");
    CHECK(formatAge(42s) == "42s ago");
    CHECK(formatAge(59s) == "59s ago");
    CHECK(formatAge(60s) == "1m ago");
    CHECK(formatAge(3599s) == "59m ago");
    CHECK(formatAge(1h) == "1h ago");
    CHECK(formatAge(23h + 59min) == "23h ago");
    CHECK(formatAge(std::chrono::days { 1 }) == "1d ago");
    CHECK(formatAge(std::chrono::days { 30 }) == "30d ago");

    SECTION("a clock stepped back reads as just now")
    {
        CHECK(formatAge(-5s) == "0s ago");
    }
}

TEST_CASE("recentCommandsNotice: an empty list is explained before a missing prompt", "[contour][picker]")
{
    using contour::command::PromptReadiness;
    using contour::command::recentCommandsNotice;
    using contour::command::RecentCommandsNotice;

    auto snapshot = contour::command::RecentCommandsSnapshot {};

    SECTION("no rows at all (no shell integration): NoCommands, at a prompt or not")
    {
        CHECK(recentCommandsNotice(snapshot) == RecentCommandsNotice::NoCommands);
        snapshot.readiness = PromptReadiness::AtPrompt;
        CHECK(recentCommandsNotice(snapshot) == RecentCommandsNotice::NoCommands);
    }

    SECTION("rows, but the target is not at its prompt")
    {
        snapshot.rows.push_back(ran("make", Here, 1s));
        CHECK(recentCommandsNotice(snapshot) == RecentCommandsNotice::NotAtPrompt);
    }

    SECTION("rows and a prompt: nothing to say")
    {
        snapshot.rows.push_back(ran("make", Here, 1s));
        snapshot.readiness = PromptReadiness::AtPrompt;
        CHECK(recentCommandsNotice(snapshot) == RecentCommandsNotice::None);
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'statusOf'`, `'formatAge'`, `'RecentCommandsSnapshot'`, `'recentCommandsNotice'` are not members of `contour::command`.

- [ ] **Step 3: Declare them**

In `src/contour/command/RecentCommands.hpp`, insert before the closing `} // namespace contour::command`:

```cpp
/// How a command ended, as the picker's status mark shows it.
enum class RecentCommandStatus : uint8_t
{
    Unknown = 0, ///< No exit code was reported (the next prompt closed the block).
    Succeeded,   ///< Exit code 0.
    Failed,      ///< Any other exit code.
};

/// @param exitCode The command's exit code, if one was reported.
/// @return The status mark the picker shows for it.
[[nodiscard]] constexpr RecentCommandStatus statusOf(std::optional<int> exitCode) noexcept
{
    if (!exitCode)
        return RecentCommandStatus::Unknown;
    return *exitCode == 0 ? RecentCommandStatus::Succeeded : RecentCommandStatus::Failed;
}

/// How long ago something finished, as the picker's age column reads: "42s ago", "3m ago", "5h ago",
/// "2d ago" — the largest whole unit, rounded down.
/// @param age The time since it finished; a negative age (a clock stepped back) reads "0s ago".
/// @return The text.
[[nodiscard]] std::string formatAge(std::chrono::system_clock::duration age);

/// Everything the picker shows, taken once when it opens (TerminalSessionManager::recentCommandsSnapshot).
struct RecentCommandsSnapshot
{
    std::vector<RecentCommandRow> rows; ///< Every open session's rows, each session oldest first.
    uint64_t currentSessionId {};       ///< The session the picker inserts into; 0 when there is none.
    PromptReadiness readiness = PromptReadiness::NotAtPrompt; ///< Whether that session is at its prompt.
};

/// The one line the picker shows above its rows when it cannot simply be used.
enum class RecentCommandsNotice : uint8_t
{
    None = 0,    ///< Rows to pick from and a prompt to insert at.
    NoCommands,  ///< No open session has a finished command: shell integration is likely missing.
    NotAtPrompt, ///< Rows exist, but the target is not at its prompt: accepting inserts nothing.
};

/// Which notice @p snapshot calls for. An empty list outranks the prompt check: without shell
/// integration both hold, and only the first tells the user what to do about it.
/// @param snapshot What the picker opened with.
/// @return The notice.
[[nodiscard]] RecentCommandsNotice recentCommandsNotice(RecentCommandsSnapshot const& snapshot) noexcept;
```

- [ ] **Step 4: Implement them**

In `src/contour/command/RecentCommands.cpp`, add `#include <array>` and `#include <format>` to the standard includes; add inside the existing anonymous namespace (after `ranksBefore`):

```cpp
    /// One unit of the age column: its size and the suffix it is written with.
    struct AgeUnit
    {
        std::chrono::seconds size;
        std::string_view suffix;
    };

    /// The age column's units, ascending. A new unit (weeks) is one more row.
    constexpr auto AgeUnits = std::array {
        AgeUnit { .size = std::chrono::seconds { 1 }, .suffix = "s" },
        AgeUnit { .size = std::chrono::minutes { 1 }, .suffix = "m" },
        AgeUnit { .size = std::chrono::hours { 1 }, .suffix = "h" },
        AgeUnit { .size = std::chrono::days { 1 }, .suffix = "d" },
    };
```

and insert before the closing `} // namespace contour::command`:

```cpp
std::string formatAge(std::chrono::system_clock::duration age)
{
    auto const elapsed = std::chrono::duration_cast<std::chrono::seconds>(age);
    auto const seconds = elapsed < std::chrono::seconds::zero() ? std::chrono::seconds::zero() : elapsed;

    // The largest unit that fits at least once; seconds when none does (under one second).
    auto const* unit = &AgeUnits.front();
    for (auto const& candidate: AgeUnits)
        if (candidate.size <= seconds)
            unit = &candidate;

    return std::format("{}{} ago", seconds / unit->size, unit->suffix);
}

RecentCommandsNotice recentCommandsNotice(RecentCommandsSnapshot const& snapshot) noexcept
{
    if (snapshot.rows.empty())
        return RecentCommandsNotice::NoCommands;
    if (snapshot.readiness != PromptReadiness::AtPrompt)
        return RecentCommandsNotice::NotAtPrompt;
    return RecentCommandsNotice::None;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test && out/build/clangcl-debug/bin/contour_test.exe "[picker]"`
Expected: `All tests passed` (15 test cases). `clang-format -i` the three files.

- [ ] **Step 6: Commit**

```bash
git add src/contour/command/RecentCommands.hpp src/contour/command/RecentCommands.cpp \
        src/contour/command/RecentCommands_test.cpp
git commit -F - <<'EOF'
command: decide the picker's status marks, ages and notice

statusOf() maps an exit code to its mark, formatAge() renders "3m ago" from a
unit table, and recentCommandsNotice() explains an empty picker (no shell
integration) before a session that is not at its prompt.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.4: The bytes accepting a recent command sends

**Files:**
- Modify: `src/contour/command/RecentCommands.hpp`, `src/contour/command/RecentCommands.cpp`, `src/contour/command/RecentCommands_test.cpp`

**Interfaces:**
- Consumes: C6 `sanitizeCommandLine(text, SanitizePurpose::Insert)` — drops ESC and every C0 but TAB and LF, and every C1.
- Produces:
  ```cpp
  enum class RecentCommandAccept : uint8_t { Insert = 0, Run };
  enum class RecentCommandError : uint8_t { NotAtPrompt = 0, NothingToInsert };
  struct RecentCommandInput { std::string paste; std::string_view submit; };
  [[nodiscard]] RecentCommandInput recentCommandInput(std::string_view rawCommandLine, RecentCommandAccept how);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/command/RecentCommands_test.cpp`:

```cpp
TEST_CASE("recentCommandInput: Enter inserts, Shift+Enter also runs", "[contour][picker]")
{
    using contour::command::RecentCommandAccept;
    using contour::command::recentCommandInput;

    auto const insert = recentCommandInput("make -j8", RecentCommandAccept::Insert);
    CHECK(insert.paste == "make -j8");
    CHECK(insert.submit.empty());

    auto const run = recentCommandInput("make -j8", RecentCommandAccept::Run);
    CHECK(run.paste == "make -j8");
    CHECK(run.submit == "\r");
}

TEST_CASE("recentCommandInput: a hostile command line cannot leave the paste", "[contour][picker]")
{
    using contour::command::RecentCommandAccept;
    using contour::command::recentCommandInput;

    SECTION("an embedded paste-end sequence loses its ESC, and its CR presses nothing")
    {
        // Review focus #3: a crafted cmdline_url carrying ESC [201~ would end bracketed paste early, and
        // the shell would then RUN "rm -rf ~" instead of only showing it.
        auto const input = recentCommandInput("echo hi\x1b[201~rm -rf ~\r", RecentCommandAccept::Insert);
        CHECK(input.paste.find('\x1b') == std::string::npos);
        CHECK(input.paste.find('\r') == std::string::npos);
        CHECK(input.paste.find("rm -rf ~") != std::string::npos); // shown to the user, never executed
        CHECK(input.submit.empty());
    }

    SECTION("a multi-line command keeps its line breaks and tabs for the shell to edit")
    {
        auto const input = recentCommandInput("for f in *; do\n\techo $f\ndone", RecentCommandAccept::Insert);
        CHECK(input.paste == "for f in *; do\n\techo $f\ndone");
    }

    SECTION("a line of nothing but control characters leaves nothing to paste")
    {
        CHECK(recentCommandInput("\x1b\x01\x02", RecentCommandAccept::Run).paste.empty());
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'RecentCommandAccept'` / `'recentCommandInput'` are not members of `contour::command`.

- [ ] **Step 3: Declare them**

In `src/contour/command/RecentCommands.hpp`, insert before the closing `} // namespace contour::command`:

```cpp
/// How accepting a recent command delivers it. CommandPalette.qml's `Accept` enum mirrors these values,
/// and WindowController::acceptRecentCommand() receives them as an int.
enum class RecentCommandAccept : uint8_t
{
    Insert = 0, ///< Enter: put the command at the prompt, for the user to edit or run.
    Run,        ///< Shift+Enter: put it at the prompt and press Enter.
};

/// Why a recent command was not inserted.
enum class RecentCommandError : uint8_t
{
    NotAtPrompt = 0, ///< The target session is not at its prompt (see promptReadinessOf()).
    NothingToInsert, ///< Nothing is left of the command line once insertion has stripped it.
};

/// The bytes accepting a recent command sends to the shell.
struct RecentCommandInput
{
    std::string paste;       ///< The command line sanitised for insertion; goes through the paste path.
    std::string_view submit; ///< Sent raw after the paste: "\r" to run it, empty to leave it at the prompt.
};

/// What accepting @p rawCommandLine sends (spec §9, §10.4).
///
/// The command line came from whatever wrote to the pty, so it is sanitised with SanitizePurpose::Insert:
/// ESC and every other control character but TAB and LF are dropped. That is what stops a crafted
/// `cmdline_url` carrying `ESC [201~` from ending the shell's bracketed paste early and running the rest.
/// The `\r` of a Run is ours, sent after the paste and therefore outside the brackets, which is what makes
/// the shell execute the line.
/// @param rawCommandLine The command line as the store recorded it.
/// @param how            Whether to only insert it or also run it.
/// @return The paste and what follows it.
[[nodiscard]] RecentCommandInput recentCommandInput(std::string_view rawCommandLine, RecentCommandAccept how);
```

- [ ] **Step 4: Implement it**

In `src/contour/command/RecentCommands.cpp`, add inside the anonymous namespace (after `AgeUnits`):

```cpp
    /// What follows the paste for @p how.
    [[nodiscard]] constexpr std::string_view submitSequenceFor(RecentCommandAccept how) noexcept
    {
        switch (how)
        {
            case RecentCommandAccept::Insert: return {};
            case RecentCommandAccept::Run: return "\r";
        }
        return {};
    }
```

and insert before the closing `} // namespace contour::command`:

```cpp
RecentCommandInput recentCommandInput(std::string_view rawCommandLine, RecentCommandAccept how)
{
    return RecentCommandInput {
        .paste = vtbackend::sanitizeCommandLine(rawCommandLine, vtbackend::SanitizePurpose::Insert),
        .submit = submitSequenceFor(how),
    };
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test && out/build/clangcl-debug/bin/contour_test.exe "[picker]"`
Expected: `All tests passed` (17 test cases). `clang-format -i` the three files.

- [ ] **Step 6: Commit**

```bash
git add src/contour/command/RecentCommands.hpp src/contour/command/RecentCommands.cpp \
        src/contour/command/RecentCommands_test.cpp
git commit -F - <<'EOF'
command: sanitise a recalled command line for insertion

recentCommandInput() strips ESC and control characters before the paste, so an
embedded ESC [201~ cannot end bracketed paste early, and appends the Enter of
a Shift+Enter outside the paste.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.5: Share the UTF-8 → UTF-16 highlight-index translation

**Files:**
- Modify: `src/contour/command/FuzzyFilter.hpp`, `src/contour/command/FuzzyFilter_test.cpp`, `src/contour/window/CommandPaletteModel.cpp:29-69,246`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  ```cpp
  template <std::integral Offset>
  [[nodiscard]] std::vector<int> utf16Indices(std::string_view text, std::span<Offset const> byteOffsets);
  ```

`CommandPaletteModel.cpp:36-69` holds `byteOffsetsToUtf16` in an anonymous namespace. The recent-commands model (Task 9.6) needs the same translation for `size_t` positions; moving it next to `fuzzyMatch()` (whose output it translates) avoids a second copy. The palette's behaviour and its tests are unchanged — `CommandPaletteModel_test.cpp:270-301` keeps guarding it.

- [ ] **Step 1: Write the failing test**

Append to `src/contour/command/FuzzyFilter_test.cpp`:

```cpp
TEST_CASE("utf16Indices translates UTF-8 byte offsets to UTF-16 code units", "[contour][palette]")
{
    // U+2192 (→) is three UTF-8 bytes but one UTF-16 unit; U+1F600 is four bytes and a surrogate pair.
    auto const text = std::string_view { "\xE2\x86\x92z\xF0\x9F\x98\x80y" };

    SECTION("int offsets, as fuzzyMatch() reports them")
    {
        auto const offsets = std::vector<int> { 0, 3, 8 };
        CHECK(utf16Indices<int>(text, offsets) == std::vector<int> { 0, 1, 4 });
    }

    SECTION("size_t offsets, as the recent-commands ranking reports them")
    {
        auto const offsets = std::vector<std::size_t> { 3, 8 };
        CHECK(utf16Indices<std::size_t>(text, offsets) == std::vector<int> { 1, 4 });
    }

    SECTION("an offset that is negative, inside a character, or past the end is dropped")
    {
        auto const offsets = std::vector<int> { -1, 1, 3, 10 };
        CHECK(utf16Indices<int>(text, offsets) == std::vector<int> { 1 });
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'utf16Indices': identifier not found`.

- [ ] **Step 3: Move the translation into `FuzzyFilter.hpp`**

In `src/contour/command/FuzzyFilter.hpp`, replace the includes

```cpp
#include <optional>
#include <string_view>
#include <vector>
```

with

```cpp
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
```

and insert before the closing `} // namespace contour::command`:

```cpp
/// Translates match offsets from UTF-8 byte positions (what fuzzyMatch() reports) into UTF-16 code-unit
/// indices (what a QML string is indexed by).
///
/// For all-ASCII text the two coincide, but a multibyte character shifts every later index, so without
/// this a view would emphasize the wrong characters of a non-ASCII title or command line.
///
/// @tparam Offset      The integer type the offsets are held in (int from fuzzyMatch(), size_t from
///                     rankRecentCommands()).
/// @param text         The UTF-8 text the offsets point into.
/// @param byteOffsets  Ascending byte offsets at code-point boundaries.
/// @return The UTF-16 indices, ascending. A negative offset, one inside a character and one past the end
///         of @p text are dropped.
template <std::integral Offset>
[[nodiscard]] std::vector<int> utf16Indices(std::string_view text, std::span<Offset const> byteOffsets)
{
    auto result = std::vector<int> {};
    result.reserve(byteOffsets.size());

    auto bytePos = std::size_t { 0 };
    auto utf16Pos = 0;
    for (auto const offset: byteOffsets)
    {
        if (std::cmp_less(offset, 0))
            continue; // defensive: ignore a nonsensical offset rather than loop on it
        auto const target = static_cast<std::size_t>(offset);
        // Offsets are ascending, so bytePos only ever moves forward: walk to the target one whole UTF-8
        // code point at a time, accumulating each one's UTF-16 width (2 for a 4-byte code point, which is
        // a surrogate pair, else 1).
        while (bytePos < target && bytePos < text.size())
        {
            auto const lead = static_cast<unsigned char>(text[bytePos]);
            auto seqLen = 1;
            if (lead >= 0xF0)
                seqLen = 4;
            else if (lead >= 0xE0)
                seqLen = 3;
            else if (lead >= 0x80)
                seqLen = 2;
            bytePos += static_cast<std::size_t>(seqLen);
            utf16Pos += seqLen == 4 ? 2 : 1;
        }
        if (bytePos == target)
            result.push_back(utf16Pos);
    }
    return result;
}
```

In `src/contour/window/CommandPaletteModel.cpp`, delete the whole `byteOffsetsToUtf16` function with its doc comment (lines 29-69, from `    /// Translates fuzzy-match offsets from UTF-8 byte positions` through its closing `    }`), leaving `byTitle` alone in the anonymous namespace, and replace (line 246)

```cpp
            auto const utf16Matches = byteOffsetsToUtf16(entry.command->title, entry.titleMatches);
```

with

```cpp
            auto const utf16Matches = command::utf16Indices<int>(entry.command->title, entry.titleMatches);
```

- [ ] **Step 4: Run both suites to verify**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test && out/build/clangcl-debug/bin/contour_test.exe "[palette]" && out/build/clangcl-debug/bin/contour_gui_test.exe "[palette]"`
Expected: both `All tests passed`; the palette's UTF-16 regression test (`Title-match highlight indices are UTF-16 code units, not UTF-8 byte offsets`) passes unedited. Then `clang-format -i src/contour/command/FuzzyFilter.hpp src/contour/command/FuzzyFilter_test.cpp src/contour/window/CommandPaletteModel.cpp`.

- [ ] **Step 5: Commit**

```bash
git add src/contour/command/FuzzyFilter.hpp src/contour/command/FuzzyFilter_test.cpp \
        src/contour/window/CommandPaletteModel.cpp
git commit -F - <<'EOF'
command: share the UTF-8 to UTF-16 highlight-index translation

Moves the palette's private byte-offset translation next to fuzzyMatch() as
utf16Indices(), so the recent-commands model can reuse it for its size_t match
positions. The palette's behaviour is unchanged.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.6: `RecentCommandsModel` — the picker's list model

**Files:**
- Create: `src/contour/window/RecentCommandsModel.hpp`, `src/contour/window/RecentCommandsModel.cpp`, `src/contour/window/RecentCommandsModel_test.cpp`
- Modify: `src/contour/CMakeLists.txt` (`_header_files` GUI list at :225-249, `_core_source_files` GUI list at :250-273, `contour_gui_test` sources at :598-656), `src/contour/ContourGuiApp.cpp:1140,1145` (QML type registration)

**Interfaces:**
- Consumes: Tasks 9.1–9.5 (including `RecentCommandRow::locality`, Task 9.2); `[[nodiscard]] std::string vtbackend::abbreviateHomePath(std::string_view path, std::string_view home);` (`src/vtbackend/core/WorkingDirectory.hpp`, moved there from `contour::window` by phase 6 Task 6.1 — the window-layer copy no longer exists); `core::platform::WallClockRef` (`vendor/core-cpp/src/core/platform/Clock.hpp:283-303`); C6 `sanitizeCommandLine(..., SanitizePurpose::Display)`.
- Produces:
  ```cpp
  class RecentCommandsModel: public QAbstractListModel   // namespace contour::window
  {
      Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
      Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    public:
      enum class Roles : std::uint16_t { CommandRole = Qt::UserRole + 1, CommandMatchesRole, StatusRole,
                                         DirectoryRole, AgeRole, TabNameRole, CountRole };
      RecentCommandsModel(core::platform::WallClockRef wallClock, std::string homeDirectory, QObject* parent = nullptr);
      void refresh(command::RecentCommandsSnapshot snapshot);
      [[nodiscard]] std::optional<std::string> commandLineAt(int row) const;          // RAW
      [[nodiscard]] command::RecentCommandsNotice noticeKind() const noexcept;
      [[nodiscard]] QString filter() const;  void setFilter(QString const& filter);
      [[nodiscard]] QString notice() const;
    signals: void filterChanged(); void noticeChanged();
  };
  ```
  QML role names: `command`, `commandMatches`, `exitStatus`, `directory`, `age`, `tabName`, `runCount`.

The model reads no terminal: it ranks the snapshot it was handed (`refresh`) against its filter, so typing never takes a terminal lock and a session closing under the open picker cannot pull a row away. The wall clock is read once per `refresh` (configuration at construction: clock and home directory are fixed). Every untrusted string it displays — command line and working directory — goes through `SanitizePurpose::Display`.

- [ ] **Step 1: Write the failing tests**

Create `src/contour/window/RecentCommandsModel_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The recent-commands picker's list model: the roles QML binds, the fuzzy filter, and the notice.
// Driven with a hand-made snapshot and a wall clock the test sets, so ages and orders are exact.

#include <contour/command/RecentCommands.hpp>
#include <contour/window/RecentCommandsModel.hpp>

#include <core/platform/Clock.hpp>

#include <vtbackend/core/TerminalContext.hpp>

#include <QtCore/QChar>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QAbstractItemModelTester>
#include <QtTest/QSignalSpy>

using namespace std::chrono_literals;
using contour::command::PromptReadiness;
using contour::command::RecentCommandRow;
using contour::command::RecentCommandsNotice;
using contour::command::RecentCommandsSnapshot;
using contour::command::RecentCommandStatus;
using contour::window::RecentCommandsModel;
using Roles = contour::window::RecentCommandsModel::Roles;

namespace
{
constexpr auto Epoch = std::chrono::system_clock::time_point { std::chrono::seconds { 1'700'000'000 } };
constexpr auto Here = uint64_t { 1 };
constexpr auto There = uint64_t { 2 };

/// A finished command of session @p sessionId, @p at after the epoch, run in ~/src/contour.
[[nodiscard]] RecentCommandRow ran(std::string commandLine,
                                   uint64_t sessionId,
                                   std::chrono::seconds at,
                                   std::optional<int> exitCode,
                                   std::string tabName)
{
    return RecentCommandRow { .commandLine = std::move(commandLine),
                              .exitCode = exitCode,
                              .workingDirectory = "/home/user/src/contour",
                              .finishedAt = Epoch + at,
                              .sessionId = sessionId,
                              .tabName = std::move(tabName) };
}

/// Two commands here, one in the tab "build". Read at Epoch + 10 min, they are 3, 9 and 5 minutes old.
[[nodiscard]] RecentCommandsSnapshot standardSnapshot(PromptReadiness readiness)
{
    return RecentCommandsSnapshot {
        .rows = { ran("git status", Here, 60s, 0, "zsh"),
                  ran("make test", There, 300s, 2, "build"),
                  ran("ls", Here, 420s, std::nullopt, "zsh") },
        .currentSessionId = Here,
        .readiness = readiness,
    };
}

[[nodiscard]] QVariant roleAt(RecentCommandsModel const& model, int row, Roles role)
{
    return model.data(model.index(row, 0), static_cast<int>(role));
}

[[nodiscard]] std::string textAt(RecentCommandsModel const& model, int row, Roles role)
{
    return roleAt(model, row, role).toString().toStdString();
}

[[nodiscard]] std::vector<int> intsAt(RecentCommandsModel const& model, int row, Roles role)
{
    auto result = std::vector<int> {};
    for (auto const& value: roleAt(model, row, role).toList())
        result.push_back(value.toInt());
    return result;
}
} // namespace

TEST_CASE("RecentCommandsModel: rows carry what the picker shows", "[contour][picker]")
{
    auto wall = core::platform::ManualWallClock { Epoch + 600s };
    auto model = RecentCommandsModel { wall, "/home/user" };
    QAbstractItemModelTester const tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);

    model.refresh(standardSnapshot(PromptReadiness::AtPrompt));
    REQUIRE(model.rowCount() == 3);

    SECTION("this session's commands first, newest first; then the others")
    {
        CHECK(textAt(model, 0, Roles::CommandRole) == "ls");
        CHECK(textAt(model, 1, Roles::CommandRole) == "git status");
        CHECK(textAt(model, 2, Roles::CommandRole) == "make test");
    }

    SECTION("status, directory and age")
    {
        CHECK(roleAt(model, 0, Roles::StatusRole).toInt() == static_cast<int>(RecentCommandStatus::Unknown));
        CHECK(roleAt(model, 1, Roles::StatusRole).toInt() == static_cast<int>(RecentCommandStatus::Succeeded));
        CHECK(roleAt(model, 2, Roles::StatusRole).toInt() == static_cast<int>(RecentCommandStatus::Failed));
        CHECK(textAt(model, 0, Roles::DirectoryRole) == "~/src/contour");
        CHECK(textAt(model, 0, Roles::AgeRole) == "3m ago");
        CHECK(textAt(model, 1, Roles::AgeRole) == "9m ago");
        CHECK(textAt(model, 2, Roles::AgeRole) == "5m ago");
    }

    SECTION("the tab name is shown for another session's command only")
    {
        CHECK(textAt(model, 0, Roles::TabNameRole).empty());
        CHECK(textAt(model, 2, Roles::TabNameRole) == "build");
    }

    SECTION("a directory on another machine is not shortened against this one's home")
    {
        // `~` names THIS machine's home; the finish notification shows the same path the same way (phase 6).
        auto remote = ran("uptime", Here, 480s, 0, "zsh");
        remote.locality = vtbackend::ContextLocality::Foreign;
        model.refresh(RecentCommandsSnapshot { .rows = { std::move(remote) },
                                               .currentSessionId = Here,
                                               .readiness = PromptReadiness::AtPrompt });

        REQUIRE(model.rowCount() == 1);
        CHECK(textAt(model, 0, Roles::DirectoryRole) == "/home/user/src/contour");
    }

    SECTION("the raw command line is what accepting a row inserts")
    {
        CHECK(model.commandLineAt(2) == std::optional<std::string> { "make test" });
        CHECK_FALSE(model.commandLineAt(3).has_value());
        CHECK_FALSE(model.commandLineAt(-1).has_value());
    }

    SECTION("no notice when there is something to pick and somewhere to insert it")
    {
        CHECK(model.noticeKind() == RecentCommandsNotice::None);
        CHECK(model.notice().isEmpty());
    }
}

TEST_CASE("RecentCommandsModel: repeated commands collapse into one counted row", "[contour][picker]")
{
    auto wall = core::platform::ManualWallClock { Epoch + 600s };
    auto model = RecentCommandsModel { wall, "/home/user" };

    model.refresh(RecentCommandsSnapshot {
        .rows = { ran("make", Here, 10s, 0, "zsh"), ran("make", Here, 20s, 2, "zsh"), ran("make", There, 30s, 0, "b") },
        .currentSessionId = Here,
        .readiness = PromptReadiness::AtPrompt,
    });

    REQUIRE(model.rowCount() == 1);
    CHECK(roleAt(model, 0, Roles::CountRole).toInt() == 3);
    // The row reports the newest run HERE, which failed.
    CHECK(roleAt(model, 0, Roles::StatusRole).toInt() == static_cast<int>(RecentCommandStatus::Failed));
}

TEST_CASE("RecentCommandsModel: typing filters through the fuzzy matcher", "[contour][picker]")
{
    auto wall = core::platform::ManualWallClock { Epoch + 600s };
    auto model = RecentCommandsModel { wall, "/home/user" };
    QAbstractItemModelTester const tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.refresh(standardSnapshot(PromptReadiness::AtPrompt));

    model.setFilter(QStringLiteral("mt"));

    REQUIRE(model.rowCount() == 1);
    CHECK(textAt(model, 0, Roles::CommandRole) == "make test");
    // "mt" lands on the M of make and the T that starts "test".
    CHECK(intsAt(model, 0, Roles::CommandMatchesRole) == std::vector<int> { 0, 5 });

    SECTION("an unfiltered row highlights nothing")
    {
        model.setFilter(QString {});
        REQUIRE(model.rowCount() == 3);
        CHECK(intsAt(model, 0, Roles::CommandMatchesRole).empty());
    }

    SECTION("the next open starts unfiltered")
    {
        QSignalSpy const filterChanged(&model, &RecentCommandsModel::filterChanged);
        model.refresh(standardSnapshot(PromptReadiness::AtPrompt));
        CHECK(model.filter().isEmpty());
        CHECK(filterChanged.count() == 1);
        CHECK(model.rowCount() == 3);
    }
}

TEST_CASE("RecentCommandsModel: a command line is displayed sanitised", "[contour][picker]")
{
    // Review focus #3: the row shows control characters as placeholders, never raw; the RAW line is
    // still what accepting hands to the insertion path (which sanitises it for insertion).
    auto wall = core::platform::ManualWallClock { Epoch + 600s };
    auto model = RecentCommandsModel { wall, "/home/user" };
    auto const raw = std::string { "printf 'x'\x1b]0;pwned\x07" };

    model.refresh(RecentCommandsSnapshot { .rows = { ran(raw, Here, 10s, 0, "zsh") },
                                           .currentSessionId = Here,
                                           .readiness = PromptReadiness::AtPrompt });

    REQUIRE(model.rowCount() == 1);
    auto const shown = roleAt(model, 0, Roles::CommandRole).toString();
    CHECK_FALSE(shown.contains(QChar(0x1b)));
    CHECK_FALSE(shown.contains(QChar(0x07)));
    CHECK(model.commandLineAt(0) == std::optional<std::string> { raw });
}

TEST_CASE("RecentCommandsModel: the notice says why the picker cannot help", "[contour][picker]")
{
    auto wall = core::platform::ManualWallClock { Epoch + 600s };
    auto model = RecentCommandsModel { wall, "/home/user" };
    QSignalSpy const noticeChanged(&model, &RecentCommandsModel::noticeChanged);

    SECTION("no finished command anywhere: point at shell integration (review focus #5)")
    {
        model.refresh(RecentCommandsSnapshot {});
        CHECK(model.rowCount() == 0);
        CHECK(model.noticeKind() == RecentCommandsNotice::NoCommands);
        CHECK(model.notice().contains(QStringLiteral("shell integration")));
        CHECK(model.notice().contains(QStringLiteral("contour generate integration")));
        CHECK(noticeChanged.count() == 1);
    }

    SECTION("rows, but the target is not at a shell prompt")
    {
        model.refresh(standardSnapshot(PromptReadiness::NotAtPrompt));
        CHECK(model.rowCount() == 3);
        CHECK(model.noticeKind() == RecentCommandsNotice::NotAtPrompt);
        CHECK(model.notice().contains(QStringLiteral("Not at a shell prompt")));
    }
}
```

Register it: in `src/contour/CMakeLists.txt`, in `add_executable(contour_gui_test ...)`, add after `window/CommandPaletteModel_test.cpp`:

```cmake
            window/RecentCommandsModel_test.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `cannot open include file 'contour/window/RecentCommandsModel.hpp'`.

- [ ] **Step 3: Write the header**

Create `src/contour/window/RecentCommandsModel.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/RecentCommands.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <QtQmlIntegration/QtQmlIntegration>

namespace contour::window
{

/// The list the recent-commands picker shows: every open session's finished shell commands, collapsed
/// and ordered by command::rankRecentCommands() (spec §9).
///
/// It reads no terminal. refresh() hands it the snapshot the session manager took when the picker opened,
/// and everything after that — filtering, ranking, every row's text — is computed from that snapshot. So
/// typing never takes a terminal lock, and a session closing under the open picker cannot pull a row away.
class RecentCommandsModel: public QAbstractListModel
{
    Q_OBJECT
    /// What the user typed. Writing it re-ranks the rows.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    /// The line the picker shows above its rows — why it cannot simply be used — or empty.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    QML_ELEMENT
    QML_UNCREATABLE("Created by the window controller")

  public:
    enum class Roles : std::uint16_t
    {
        CommandRole = Qt::UserRole + 1, //!< The command line, sanitised for display.
        CommandMatchesRole,             //!< Matched command indices (UTF-16 code units) for highlighting.
        StatusRole,                     //!< command::RecentCommandStatus, as an int.
        DirectoryRole,                  //!< Where it ran, sanitised for display; home is `~` unless Foreign.
        AgeRole,                        //!< How long ago it finished, e.g. "3m ago".
        TabNameRole,                    //!< The hosting tab's label for another session's command; else empty.
        CountRole,                      //!< How many runs collapsed into the row.
    };

    /// @param wallClock     Read once per refresh(), to age the rows. Must outlive this model.
    /// @param homeDirectory Abbreviated to `~` in the directory column, except in a directory on another
    ///                      machine (ContextLocality::Foreign); empty abbreviates nothing.
    /// @param parent        Qt parent.
    RecentCommandsModel(core::platform::WallClockRef wallClock,
                        std::string homeDirectory,
                        QObject* parent = nullptr);

    /// Replaces the rows with @p snapshot, clears the filter and re-decides the notice. Called each time
    /// the picker opens.
    /// @param snapshot What the session manager collected for this opening.
    void refresh(command::RecentCommandsSnapshot snapshot);

    /// The RAW command line of the row at @p row — what accepting it inserts (sanitised there).
    /// @param row A row of the current, filtered list.
    /// @return The command line, or nullopt for a row that does not exist.
    [[nodiscard]] std::optional<std::string> commandLineAt(int row) const;

    /// Which notice the picker shows; None when it shows none.
    [[nodiscard]] command::RecentCommandsNotice noticeKind() const noexcept { return _noticeKind; }

    // {{{ QAbstractListModel
    [[nodiscard]] QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] int rowCount(QModelIndex const& parent = QModelIndex()) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    // }}}

    /// What the user typed.
    [[nodiscard]] QString filter() const { return _filter; }
    /// Sets the filter and re-ranks the rows. A no-op when @p filter is unchanged.
    void setFilter(QString const& filter);

    /// The notice as the picker shows it, translated; empty for RecentCommandsNotice::None.
    [[nodiscard]] QString notice() const;

  signals:
    void filterChanged();
    void noticeChanged();

  private:
    /// Re-ranks the snapshot against the current filter.
    void rebuildRows();

    core::platform::WallClockRef _wallClock;
    std::string _homeDirectory;

    command::RecentCommandsSnapshot _snapshot;
    std::vector<command::RankedRecentCommand> _rows;
    std::chrono::system_clock::time_point _now {}; //!< When the snapshot was taken; the rows age from it.
    command::RecentCommandsNotice _noticeKind = command::RecentCommandsNotice::None;
    QString _filter;
};

} // namespace contour::window
```

- [ ] **Step 4: Write the implementation**

Create `src/contour/window/RecentCommandsModel.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <contour/command/FuzzyFilter.hpp>
#include <contour/window/RecentCommandsModel.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <cstddef>
#include <string_view>
#include <utility>

namespace contour::window
{

namespace
{
    /// An untrusted string as the picker shows it: control characters and bidi overrides become visible
    /// placeholders (spec §10.4).
    [[nodiscard]] std::string displayed(std::string_view untrusted)
    {
        return vtbackend::sanitizeCommandLine(untrusted, vtbackend::SanitizePurpose::Display);
    }
} // namespace

RecentCommandsModel::RecentCommandsModel(core::platform::WallClockRef wallClock,
                                         std::string homeDirectory,
                                         QObject* parent):
    QAbstractListModel { parent }, _wallClock { wallClock }, _homeDirectory { std::move(homeDirectory) }
{
}

void RecentCommandsModel::refresh(command::RecentCommandsSnapshot snapshot)
{
    _snapshot = std::move(snapshot);
    _now = _wallClock.now();
    _noticeKind = command::recentCommandsNotice(_snapshot);

    // Clearing the filter here rather than making the caller do it: the same single-rebuild reasoning as
    // CommandPaletteModel::refresh().
    auto const hadFilter = !_filter.isEmpty();
    _filter.clear();

    rebuildRows();

    if (hadFilter)
        emit filterChanged();
    emit noticeChanged();
}

void RecentCommandsModel::setFilter(QString const& filter)
{
    if (_filter == filter)
        return;

    _filter = filter;
    rebuildRows();
    emit filterChanged();
}

void RecentCommandsModel::rebuildRows()
{
    // A full reset, as CommandPaletteModel does: a re-rank can move any row anywhere.
    beginResetModel();
    _rows = command::rankRecentCommands(_snapshot.rows, _snapshot.currentSessionId, _filter.toStdString());
    endResetModel();
}

std::optional<std::string> RecentCommandsModel::commandLineAt(int row) const
{
    if (row < 0 || static_cast<std::size_t>(row) >= _rows.size())
        return std::nullopt;
    return _rows[static_cast<std::size_t>(row)].newest.commandLine;
}

int RecentCommandsModel::rowCount(QModelIndex const& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(_rows.size());
}

QVariant RecentCommandsModel::data(QModelIndex const& index, int role) const
{
    auto const row = index.row();
    if (row < 0 || static_cast<std::size_t>(row) >= _rows.size())
        return {};

    auto const& entry = _rows[static_cast<std::size_t>(row)];
    auto const& newest = entry.newest;

    switch (static_cast<Roles>(role))
    {
        case Roles::CommandRole: return QString::fromStdString(displayed(newest.commandLine));
        case Roles::CommandMatchesRole: {
            // The ranking reports UTF-8 byte offsets into the DISPLAYED line; QML indexes it by UTF-16
            // code unit.
            auto matches = QVariantList {};
            for (auto const position:
                 command::utf16Indices<std::size_t>(displayed(newest.commandLine), entry.matchPositions))
                matches.append(position);
            return matches;
        }
        case Roles::StatusRole: return static_cast<int>(command::statusOf(newest.exitCode));
        case Roles::DirectoryRole:
            // `~` names THIS machine's home: a directory on another one is shown as is, exactly as the
            // finish notification shows it (phase 6), so one record never reads as two different paths.
            return QString::fromStdString(
                displayed(newest.locality == vtbackend::ContextLocality::Foreign
                              ? newest.workingDirectory
                              : vtbackend::abbreviateHomePath(newest.workingDirectory, _homeDirectory)));
        case Roles::AgeRole: return QString::fromStdString(command::formatAge(_now - newest.finishedAt));
        case Roles::TabNameRole:
            // This session's own rows need no tab: the user is looking at it.
            if (newest.sessionId == _snapshot.currentSessionId)
                return QString {};
            return QString::fromStdString(newest.tabName);
        case Roles::CountRole: return static_cast<int>(entry.count);
    }
    return {};
}

QHash<int, QByteArray> RecentCommandsModel::roleNames() const
{
    return {
        { static_cast<int>(Roles::CommandRole), "command" },
        { static_cast<int>(Roles::CommandMatchesRole), "commandMatches" },
        { static_cast<int>(Roles::StatusRole), "exitStatus" },
        { static_cast<int>(Roles::DirectoryRole), "directory" },
        { static_cast<int>(Roles::AgeRole), "age" },
        { static_cast<int>(Roles::TabNameRole), "tabName" },
        { static_cast<int>(Roles::CountRole), "runCount" },
    };
}

QString RecentCommandsModel::notice() const
{
    switch (_noticeKind)
    {
        case command::RecentCommandsNotice::None: return {};
        case command::RecentCommandsNotice::NoCommands:
            return tr("No finished commands yet. The picker lists what your shell reports through shell "
                      "integration; set it up with: contour generate integration shell SHELL to FILE");
        case command::RecentCommandsNotice::NotAtPrompt:
            return tr("Not at a shell prompt: accepting a command inserts nothing.");
    }
    return {};
}

} // namespace contour::window
```

Register the sources in `src/contour/CMakeLists.txt`: in the GUI `list(APPEND _header_files ...)` add after `window/CommandPaletteModel.hpp`:

```cmake
        window/RecentCommandsModel.hpp
```

and in the GUI `list(APPEND _core_source_files ...)` add after `window/CommandPaletteModel.cpp`:

```cmake
        window/RecentCommandsModel.cpp
```

Register the QML type in `src/contour/ContourGuiApp.cpp`: add `#include <contour/window/RecentCommandsModel.hpp>` after `#include <contour/window/CommandPaletteModel.hpp>` (line 19), add after line 1140 (`qmlRegisterUncreatableType<window::CommandPaletteModel>...`):

```cpp
    qmlRegisterUncreatableType<window::RecentCommandsModel>("Contour.Terminal", 1, 0, "RecentCommandsModel", "Created by the window controller.");
```

and after line 1145 (`qRegisterMetaType<window::CommandPaletteModel*>...`):

```cpp
    qRegisterMetaType<window::RecentCommandsModel*>("RecentCommandsModel*");
```

(Both inside the existing `// clang-format off` block, matching its one-line style.)

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && out/build/clangcl-debug/bin/contour_gui_test.exe "[picker]"`
Expected: `All tests passed` (5 test cases). If the binary exits `0xC0000135`, set the Qt `PATH`/`QT_QPA_PLATFORM_PLUGIN_PATH` as the README describes and rerun. `clang-format -i` the three new files.

- [ ] **Step 6: Commit**

```bash
git add src/contour/window/RecentCommandsModel.hpp src/contour/window/RecentCommandsModel.cpp \
        src/contour/window/RecentCommandsModel_test.cpp src/contour/CMakeLists.txt src/contour/ContourGuiApp.cpp
git commit -F - <<'EOF'
window: add the recent-commands picker's list model

RecentCommandsModel ranks a snapshot of every session's finished commands
against the typed filter and exposes command, matches, exit status,
shortened directory, age, tab name and run count to QML, plus a notice for an
empty list or a session that is not at its prompt.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.7: `TerminalSession` — prompt readiness and insertion through the paste path

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp` (includes; after `executeAction` at :863; private helpers at :948), `src/contour/session/TerminalSession.cpp` (after `executeAction` at :3657-3661), `src/contour/test/GuiTestFixtures.hpp` (after `mockPtyOf` at :86-92), `src/contour/session/TerminalSession_test.cpp` (append)

**Interfaces:**
- Consumes: Tasks 9.2, 9.4; C1 `Terminal::commandBlocks() const`; `Terminal::screenType()` (`Terminal.hpp:1429`), `Terminal::sendPaste(std::string_view)` (`Terminal.cpp:1571-1581`, wraps in `ESC[200~`/`ESC[201~` when DECSET 2004 is on, `InputGenerator.cpp:1104-1118`), `Terminal::sendRawInput(std::string_view)` (`Terminal.cpp:1583-1591`).
- Produces:
  ```cpp
  // TerminalSession, public:
  [[nodiscard]] command::PromptReadiness promptReadiness() const;
  [[nodiscard]] std::expected<void, command::RecentCommandError> insertRecentCommand(std::string_view rawCommandLine,
                                                                                     command::RecentCommandAccept how);
  // contour::test (GuiTestFixtures.hpp):
  inline void showShellPrompt(contour::session::TerminalSession& session);
  inline void runShellCommand(contour::session::TerminalSession& session, std::string_view encodedCommandLine, int exitCode);
  ```

The prompt check and the write happen under one hold of the terminal's (non-recursive) state lock, exactly as `operator()(actions::PasteSelection)` writes (`TerminalSession.cpp:2857-2874`), so the shell cannot start a command between them. `sendPaste` takes only the separate input mutex internally, the established lock order.

- [ ] **Step 1: Add the shell-integration test helpers**

In `src/contour/test/GuiTestFixtures.hpp`, add `#include <format>` to the standard includes (after `#include <filesystem>`), and insert after the closing `}` of `mockPtyOf` (line 92):

```cpp

/// Shows a fresh prompt on @p session's screen as an integrated shell's precmd does: OSC 133;A, the
/// prompt text, OSC 133;B. The session's newest command block is then Prompting.
inline void showShellPrompt(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
}

/// Runs one whole command block on @p session's screen as an integrated shell reports it: a prompt, OSC
/// 133;C carrying @p encodedCommandLine as kitty's percent-encoded `cmdline_url`, one line of output,
/// and OSC 133;D with @p exitCode.
inline void runShellCommand(contour::session::TerminalSession& session,
                            std::string_view encodedCommandLine,
                            int exitCode)
{
    showShellPrompt(session);
    session.terminal().writeToScreen(
        std::format("\r\n\033]133;C;cmdline_url={}\033\\output\r\n", encodedCommandLine));
    session.terminal().writeToScreen(std::format("\033]133;D;{}\033\\", exitCode));
}
```

- [ ] **Step 2: Write the failing tests**

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
// ============================================================================================
// The recent-commands picker's insertion (spec §9): through the paste path, sanitised, and only at a
// shell prompt on the primary screen.
// ============================================================================================

namespace
{
using contour::command::PromptReadiness;
using contour::command::RecentCommandAccept;
using contour::command::RecentCommandError;
using contour::test::runShellCommand;
using contour::test::showShellPrompt;
} // namespace

TEST_CASE("TerminalSession: a recalled command goes through the paste path", "[contour][session][picker]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    showShellPrompt(*session);
    REQUIRE(session->promptReadiness() == PromptReadiness::AtPrompt);
    auto& written = mockPtyOf(*session).stdinBuffer();

    SECTION("bracketed paste off: Enter inserts the text verbatim, nothing more")
    {
        written.clear();
        REQUIRE(session->insertRecentCommand("make test", RecentCommandAccept::Insert).has_value());
        CHECK(written == "make test");
    }

    SECTION("bracketed paste off: Shift+Enter presses Enter after it")
    {
        written.clear();
        REQUIRE(session->insertRecentCommand("make test", RecentCommandAccept::Run).has_value());
        CHECK(written == "make test\r");
    }

    SECTION("bracketed paste on: the shell receives one bracketed paste")
    {
        session->terminal().writeToScreen("\033[?2004h");
        written.clear();
        REQUIRE(session->insertRecentCommand("make test", RecentCommandAccept::Insert).has_value());
        CHECK(written == "\033[200~make test\033[201~");
    }

    SECTION("bracketed paste on: the Enter of a Run lands outside the brackets")
    {
        session->terminal().writeToScreen("\033[?2004h");
        written.clear();
        REQUIRE(session->insertRecentCommand("make test", RecentCommandAccept::Run).has_value());
        CHECK(written == "\033[200~make test\033[201~\r");
    }

    SECTION("a multi-line command stays one paste, so the shell cannot run its first line early")
    {
        session->terminal().writeToScreen("\033[?2004h");
        written.clear();
        REQUIRE(session->insertRecentCommand("for f in *; do\n  echo $f\ndone", RecentCommandAccept::Insert)
                    .has_value());
        CHECK(written == "\033[200~for f in *; do\n  echo $f\ndone\033[201~");
    }
}

TEST_CASE("TerminalSession: a hostile recorded command line cannot end the paste early",
          "[contour][session][picker]")
{
    // Review focus #3, end to end: the shell reports a command line whose percent-encoding hides an
    // ESC [201~ and a CR. The store keeps it RAW; inserting it must not let either reach the shell.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    runShellCommand(*session, "echo%20hi%1B%5B201~rm%20-rf%20~%0D", 0);
    showShellPrompt(*session);
    session->terminal().writeToScreen("\033[?2004h");

    auto const recorded = core::locked(session->terminal(), [&] {
        auto const* record = session->terminal().commandBlocks().lastFinished();
        return record != nullptr ? record->commandLine : std::string {};
    });
    REQUIRE(recorded == "echo hi\x1b[201~rm -rf ~\r");

    auto& written = mockPtyOf(*session).stdinBuffer();
    written.clear();
    REQUIRE(session->insertRecentCommand(recorded, RecentCommandAccept::Insert).has_value());

    CHECK(countOccurrences(written, "\033[201~") == 1); // ours, closing the paste
    CHECK(written.ends_with("\033[201~"));
    CHECK(countOccurrences(written, "\033") == 2); // the two brackets and nothing else
    CHECK(written.find('\r') == std::string::npos); // and nothing presses Enter
}

TEST_CASE("TerminalSession: a recalled command is refused away from a shell prompt",
          "[contour][session][picker]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& written = mockPtyOf(*session).stdinBuffer();

    // Inserts @p commandLine as a Run and returns why it was refused (nullopt: it was not). Whatever the
    // answer, a refusal must have written nothing at all.
    auto const refusal = [&](std::string_view commandLine) -> std::optional<RecentCommandError> {
        written.clear();
        auto const result = session->insertRecentCommand(commandLine, RecentCommandAccept::Run);
        if (result.has_value())
            return std::nullopt;
        CHECK(written.empty());
        return result.error();
    };

    SECTION("no shell integration: no prompt is known (review focus #5)")
    {
        session->terminal().writeToScreen("C:\\> ");
        CHECK(session->promptReadiness() == PromptReadiness::NotAtPrompt);
        CHECK(refusal("dir") == RecentCommandError::NotAtPrompt);
    }

    SECTION("while a command runs")
    {
        showShellPrompt(*session);
        session->terminal().writeToScreen("\r\n\033]133;C;cmdline_url=sleep%2010\033\\");
        CHECK(session->promptReadiness() == PromptReadiness::NotAtPrompt);
        CHECK(refusal("ls") == RecentCommandError::NotAtPrompt);
    }

    SECTION("on the alternate screen, and allowed again once it is left (review focus #2)")
    {
        showShellPrompt(*session);
        session->terminal().writeToScreen("\033[?1049h");
        CHECK(session->promptReadiness() == PromptReadiness::NotAtPrompt);
        CHECK(refusal("ls") == RecentCommandError::NotAtPrompt);

        session->terminal().writeToScreen("\033[?1049l");
        CHECK(session->promptReadiness() == PromptReadiness::AtPrompt);
        written.clear();
        CHECK(session->insertRecentCommand("ls", RecentCommandAccept::Insert).has_value());
        CHECK(written == "ls");
    }

    SECTION("nothing left once insertion has stripped the line")
    {
        showShellPrompt(*session);
        CHECK(refusal("\x1b\x01\x02") == RecentCommandError::NothingToInsert);
    }
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `'promptReadiness': is not a member of 'contour::session::TerminalSession'` (and `insertRecentCommand`).

- [ ] **Step 4: Declare the session API**

In `src/contour/session/TerminalSession.hpp`, add `#include <contour/command/RecentCommands.hpp>` after `#include <contour/command/ContextMenu.hpp>` and `#include <expected>` after `#include <cstdint>`. After `bool executeAction(actions::Action const& action);` (line 863) insert:

```cpp

    /// Whether the recent-commands picker may insert into this session right now: its shell waits at a
    /// prompt on the primary screen (command::promptReadinessOf()). Takes the terminal lock.
    [[nodiscard]] command::PromptReadiness promptReadiness() const;

    /// Inserts a recalled command line at this session's prompt — the recent-commands picker's accept.
    ///
    /// Goes through the paste path, so a shell with bracketed paste receives a multi-line command as one
    /// paste instead of running its first line; the line is sanitised for insertion first, and a Run
    /// presses Enter after the paste (command::recentCommandInput()). The prompt check and the write
    /// happen under one hold of the terminal lock, so the shell cannot leave its prompt in between.
    /// @param rawCommandLine The command line as the store recorded it (RAW).
    /// @param how            Whether to only insert it or also run it.
    /// @return Nothing on success; NotAtPrompt or NothingToInsert when nothing was sent (the reason is
    ///         logged too).
    [[nodiscard]] std::expected<void, command::RecentCommandError> insertRecentCommand(
        std::string_view rawCommandLine, command::RecentCommandAccept how);
```

and in the private section, after `  private:\n    // helpers` (line 948-949), insert:

```cpp

    /// promptReadiness() for a caller that ALREADY holds the terminal lock (it is not recursive).
    [[nodiscard]] command::PromptReadiness promptReadinessLocked() const noexcept;
```

- [ ] **Step 5: Implement it**

In `src/contour/session/TerminalSession.cpp`, insert after `TerminalSession::executeAction` (ends line 3661):

```cpp

command::PromptReadiness TerminalSession::promptReadiness() const
{
    auto const lock = scoped_lock { _terminal };
    return promptReadinessLocked();
}

command::PromptReadiness TerminalSession::promptReadinessLocked() const noexcept
{
    return command::promptReadinessOf(_terminal.commandBlocks(), _terminal.screenType());
}

std::expected<void, command::RecentCommandError> TerminalSession::insertRecentCommand(
    std::string_view rawCommandLine, command::RecentCommandAccept how)
{
    auto const input = command::recentCommandInput(rawCommandLine, how);

    // Checked and written under ONE hold of the lock, as PasteSelection writes: the shell cannot start a
    // command between the check and the paste.
    auto const lock = scoped_lock { _terminal };

    if (promptReadinessLocked() != command::PromptReadiness::AtPrompt)
    {
        sessionLog()("Not inserting a recent command: the shell is not at its prompt.");
        return std::unexpected(command::RecentCommandError::NotAtPrompt);
    }

    if (input.paste.empty())
    {
        sessionLog()("Not inserting a recent command: nothing is left of it once sanitised.");
        return std::unexpected(command::RecentCommandError::NothingToInsert);
    }

    _terminal.sendPaste(input.paste);
    if (!input.submit.empty())
        _terminal.sendRawInput(input.submit);
    return {};
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && out/build/clangcl-debug/bin/contour_gui_test.exe "[picker]"`
Expected: `All tests passed` (8 test cases: 5 from Task 9.6, 3 here). `clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/test/GuiTestFixtures.hpp`.

- [ ] **Step 7: Commit**

```bash
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp \
        src/contour/session/TerminalSession_test.cpp src/contour/test/GuiTestFixtures.hpp
git commit -F - <<'EOF'
session: insert a recalled command at the prompt through the paste path

insertRecentCommand() re-checks that the shell waits at its prompt on the
primary screen, sanitises the line for insertion and sends it as a paste,
with Enter after the paste for Shift+Enter. Tests cover bracketed paste on
and off, an embedded ESC [201~, and refusal while a command runs, on the
alternate screen and without shell integration.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.8: The cross-session snapshot and the window's picker API

**Files:**
- Modify: `src/contour/session/TerminalSessionManager.hpp` (after `openCommandPalette` at :199-202), `src/contour/session/TerminalSessionManager.cpp` (after `openCommandPalette` at :540-548), `src/contour/window/WindowController.hpp` (Q_PROPERTY block :80; palette block :268-283; signals :586-588; members :694-706), `src/contour/window/WindowController.cpp` (constructor :41-51; after `runCommand` :214-236), `src/contour/session/TerminalSessionManager_test.cpp` (append)

**Interfaces:**
- Consumes: Tasks 9.2, 9.3, 9.6, 9.7; `[[nodiscard]] std::string TerminalSessionManager::tabTitleForSession(TerminalSession const&) const` (phase 6, Task 6.6 — the tab strip's label for the tab hosting a session; reuse it, do not add a second label resolver); `_sessionsById`; `core::locked` (`vendor/core-cpp/src/core/Utils.hpp:452-458`).
- Produces:
  ```cpp
  // TerminalSessionManager, public:
  void openRecentCommands(TerminalSession* acting);
  [[nodiscard]] command::RecentCommandsSnapshot recentCommandsSnapshot(TerminalSession const* focused) const;
  // WindowController:
  Q_PROPERTY(contour::window::RecentCommandsModel* recentCommands READ recentCommands CONSTANT)
  [[nodiscard]] RecentCommandsModel* recentCommands() const noexcept;
  void openRecentCommands(session::TerminalSession* target);
  Q_INVOKABLE void acceptRecentCommand(int row, int how);
  signal: void recentCommandsRequested();
  ```

Ordering matters for the lock: `tabTitleForSession()` resolves the label through the window controller's tab titles, which read the session's window title under the terminal's state mutex — a plain, non-recursive `std::mutex` (`Terminal.hpp:328-330`). So each session's label is resolved *before* its lock is taken for the row copy, and `promptReadiness()` (which locks) is called outside it too.

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/session/TerminalSessionManager_test.cpp` (add `#include <contour/command/RecentCommands.hpp>` and `#include <QtCore/QString>` to its includes):

```cpp
// ============================================================================================
// The recent-commands picker (spec §9): one snapshot across every open session, ranked for the session
// it opened over, and insertion into exactly that session.
// ============================================================================================

namespace
{
using PickerRoles = contour::window::RecentCommandsModel::Roles;

/// One role of one picker row, as text.
[[nodiscard]] std::string pickerText(contour::window::RecentCommandsModel const& model, int row, PickerRoles role)
{
    return model.data(model.index(row, 0), static_cast<int>(role)).toString().toStdString();
}
} // namespace

TEST_CASE("TerminalSessionManager: the recent-commands picker lists every open session's commands",
          "[manager][picker]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* here = app.manager().createSession(win.id);
    auto* there = app.manager().createSession(win.id);
    REQUIRE(here != nullptr);
    REQUIRE(there != nullptr);
    win->setTabTitle(1, QStringLiteral("build")); // `there` is the second tab

    contour::test::runShellCommand(*here, "git%20status", 0);
    contour::test::runShellCommand(*there, "make", 2);
    contour::test::showShellPrompt(*here);
    contour::test::showShellPrompt(*there);

    app.manager().openRecentCommands(here);
    auto const& model = *win->recentCommands();

    REQUIRE(model.rowCount() == 2);
    CHECK(model.noticeKind() == contour::command::RecentCommandsNotice::None);

    SECTION("this session's command first, the other tab's after it, named by its tab")
    {
        CHECK(pickerText(model, 0, PickerRoles::CommandRole) == "git status");
        CHECK(pickerText(model, 0, PickerRoles::TabNameRole).empty());
        CHECK(pickerText(model, 1, PickerRoles::CommandRole) == "make");
        CHECK(pickerText(model, 1, PickerRoles::TabNameRole) == "build");
    }

    SECTION("accepting a row inserts into the session the picker was opened over")
    {
        auto& hereInput = contour::test::mockPtyOf(*here).stdinBuffer();
        auto& thereInput = contour::test::mockPtyOf(*there).stdinBuffer();
        hereInput.clear();
        thereInput.clear();

        win->acceptRecentCommand(1, static_cast<int>(contour::command::RecentCommandAccept::Run));

        CHECK(hereInput == "make\r");
        CHECK(thereInput.empty());
    }

    SECTION("a stale row inserts nothing")
    {
        auto& hereInput = contour::test::mockPtyOf(*here).stdinBuffer();
        hereInput.clear();
        win->acceptRecentCommand(7, static_cast<int>(contour::command::RecentCommandAccept::Insert));
        CHECK(hereInput.empty());
    }

    SECTION("opened over the other session, the ranking turns around")
    {
        app.manager().openRecentCommands(there);
        CHECK(pickerText(model, 0, PickerRoles::CommandRole) == "make");
        CHECK(pickerText(model, 0, PickerRoles::TabNameRole).empty());
        CHECK(pickerText(model, 1, PickerRoles::CommandRole) == "git status");
    }
}

TEST_CASE("TerminalSessionManager: over a busy session the picker lists but does not insert",
          "[manager][picker]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* here = app.manager().createSession(win.id);
    auto* there = app.manager().createSession(win.id);
    REQUIRE(here != nullptr);
    REQUIRE(there != nullptr);

    contour::test::runShellCommand(*there, "make", 0);
    contour::test::showShellPrompt(*there);
    // `here` runs vim: OSC 133;C, then the alternate screen.
    contour::test::showShellPrompt(*here);
    here->terminal().writeToScreen("\r\n\033]133;C;cmdline_url=vim\033\\\033[?1049h");

    app.manager().openRecentCommands(here);
    auto const& model = *win->recentCommands();

    REQUIRE(model.rowCount() == 1); // vim is still running, so only `make` is listed
    CHECK(model.noticeKind() == contour::command::RecentCommandsNotice::NotAtPrompt);

    auto& hereInput = contour::test::mockPtyOf(*here).stdinBuffer();
    hereInput.clear();
    win->acceptRecentCommand(0, static_cast<int>(contour::command::RecentCommandAccept::Run));
    CHECK(hereInput.empty());
}

TEST_CASE("TerminalSessionManager: without shell integration the picker explains instead of listing",
          "[manager][picker]")
{
    // Review focus #5: plain cmd.exe, or a shell without the script. No records anywhere.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* session = app.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    session->terminal().writeToScreen("C:\\> dir\r\nplain output\r\nC:\\> ");

    app.manager().openRecentCommands(session);
    auto const& model = *win->recentCommands();

    CHECK(model.rowCount() == 0);
    CHECK(model.noticeKind() == contour::command::RecentCommandsNotice::NoCommands);
    CHECK(model.notice().contains(QStringLiteral("shell integration")));

    auto& input = contour::test::mockPtyOf(*session).stdinBuffer();
    input.clear();
    win->acceptRecentCommand(0, static_cast<int>(contour::command::RecentCommandAccept::Insert));
    CHECK(input.empty());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `'openRecentCommands': is not a member of 'contour::session::TerminalSessionManager'`, `'recentCommands': is not a member of 'contour::window::WindowController'`.

- [ ] **Step 3: Declare the manager API**

In `src/contour/session/TerminalSessionManager.hpp`, add `#include <contour/command/RecentCommands.hpp>` after `#include <contour/command/CommandHistoryStore.hpp>` and `#include <string>` after `#include <span>`. After the `openCommandPalette` declaration (line 202) insert:

```cpp

    /// Opens the recent-commands picker over the window hosting @p acting (the OpenRecentCommands
    /// action). The picker inserts into @p acting. No-ops if @p acting has no hosting window.
    /// @param acting The session that triggered the action.
    void openRecentCommands(TerminalSession* acting);

    /// Every open session's finished commands, as the recent-commands picker shows them (spec §9).
    ///
    /// One short read per session under that terminal's lock. Sessions are visited in model-id order, so
    /// the ranking's input order does not depend on how a hash map buckets them. Daemon-attached sessions
    /// take part through their mirrored stores.
    /// @param focused The session the picker inserts into (its rows rank first, its prompt decides the
    ///                readiness); nullptr for none.
    /// @return The snapshot.
    [[nodiscard]] command::RecentCommandsSnapshot recentCommandsSnapshot(TerminalSession const* focused) const;
```

- [ ] **Step 4: Declare the window API**

In `src/contour/window/WindowController.hpp`:

add `#include <contour/window/RecentCommandsModel.hpp>` after `#include <contour/window/ContextMenuModel.hpp>`;

after the `commandPalette` Q_PROPERTY (line 80) insert:

```cpp
    /// This window's recent-commands picker list. Per-window for the same reason as the palette: the
    /// filter and the selection are things the user is doing IN this window.
    Q_PROPERTY(contour::window::RecentCommandsModel* recentCommands READ recentCommands CONSTANT)
```

after `Q_INVOKABLE void runCommand(QString const& id);` and its `// }}}` (line 282-283) insert:

```cpp

    // {{{ Recent-commands picker
    /// This window's recent-commands list model (never null; owned by this controller).
    [[nodiscard]] RecentCommandsModel* recentCommands() const noexcept { return _recentCommands.get(); }

    /// Snapshots every open session's finished commands and asks the QML to show the picker.
    ///
    /// Called by the manager for the OpenRecentCommands action. @p target is pinned: accepting a row
    /// inserts into it, not into whichever pane is active by then — the same reason the context menu pins
    /// its session.
    /// @param target The session the picker was opened over; its commands rank first.
    void openRecentCommands(session::TerminalSession* target);

    /// Inserts the command on row @p row into the session the picker was opened over.
    ///
    /// QML-facing, so the mode arrives as an int: command::RecentCommandAccept's value (0 inserts, 1 also
    /// runs); any other value inserts, the harmless choice. A stale row, a dead target, or a target no
    /// longer at its prompt inserts nothing.
    /// @param row The picker row (RecentCommandsModel's index).
    /// @param how command::RecentCommandAccept, as an int.
    Q_INVOKABLE void acceptRecentCommand(int row, int how);
    // }}}
```

after `void commandPaletteRequested();` (line 588) insert:

```cpp
    /// Requests that this window show its recent-commands picker (Main.qml answers with open()).
    void recentCommandsRequested();
```

and after `std::unique_ptr<CommandPaletteModel> _commandPalette;` (line 705) insert:

```cpp
    /// The recent-commands picker's rows, and the session it was opened over. A QPointer, so a session
    /// that dies under the open picker makes accepting a no-op rather than a use-after-free.
    std::unique_ptr<RecentCommandsModel> _recentCommands;
    QPointer<session::TerminalSession> _recentCommandsTarget;
```

- [ ] **Step 5: Implement the manager side**

In `src/contour/session/TerminalSessionManager.cpp`, add `#include <core/Utils.hpp>` after the vtpty includes and `#include <algorithm>`, `#include <functional>`, `#include <iterator>`, `#include <vector>` to the standard includes. After `TerminalSessionManager::openCommandPalette` (ends line 548) insert:

```cpp

void TerminalSessionManager::openRecentCommands(TerminalSession* acting)
{
    auto* controller = controllerHostingSession(acting);
    if (controller == nullptr)
        return;

    controller->openRecentCommands(acting);
}

command::RecentCommandsSnapshot TerminalSessionManager::recentCommandsSnapshot(TerminalSession const* focused) const
{
    auto sessions = std::vector<TerminalSession*> {};
    sessions.reserve(_sessionsById.size());
    for (auto* session: _sessionsById | std::views::values)
        if (session != nullptr)
            sessions.push_back(session);
    std::ranges::sort(sessions, std::less {}, [](TerminalSession const* session) {
        return session->modelSessionId().value;
    });

    auto snapshot = command::RecentCommandsSnapshot {};
    for (auto* session: sessions)
    {
        // The label FIRST: resolving it takes this terminal's (non-recursive) lock itself.
        auto const tabName = tabTitleForSession(*session);
        auto rows = core::locked(session->terminal(), [&] {
            return command::recentCommandRowsOf(
                session->terminal().commandBlocks(), session->modelSessionId().value, tabName);
        });
        std::ranges::move(rows, std::back_inserter(snapshot.rows));
    }

    if (focused != nullptr)
    {
        snapshot.currentSessionId = focused->modelSessionId().value;
        snapshot.readiness = focused->promptReadiness();
    }
    return snapshot;
}

```

- [ ] **Step 6: Implement the window side**

In `src/contour/window/WindowController.cpp`, add `#include <core/platform/Clock.hpp>` after the `<contour/...>` includes. In the constructor's initializer list, replace

```cpp
    _commandPalette { std::make_unique<CommandPaletteModel>(manager.commandHistory(), this) }
```

with

```cpp
    _commandPalette { std::make_unique<CommandPaletteModel>(manager.commandHistory(), this) },
    // The production wall clock and the user's home: the composition root's choices for this model,
    // fixed at construction; tests construct the model with their own.
    _recentCommands { std::make_unique<RecentCommandsModel>(
        core::platform::defaultSystemWallClock(), QDir::homePath().toStdString(), this) }
```

and after `WindowController::runCommand` (ends line 236) insert:

```cpp

void WindowController::openRecentCommands(session::TerminalSession* target)
{
    _recentCommandsTarget = target;
    _recentCommands->refresh(_manager.recentCommandsSnapshot(target));
    emit recentCommandsRequested();
}

void WindowController::acceptRecentCommand(int row, int how)
{
    auto* session = _recentCommandsTarget.data();
    if (session == nullptr)
        return;

    auto const commandLine = _recentCommands->commandLineAt(row);
    if (!commandLine)
        return;

    auto const accept = how == static_cast<int>(command::RecentCommandAccept::Run)
                            ? command::RecentCommandAccept::Run
                            : command::RecentCommandAccept::Insert;

    // The result is dropped deliberately: a refusal is the guard doing its job (the shell left its
    // prompt while the picker stood open), and the session has already logged why.
    [[maybe_unused]] auto const inserted = session->insertRecentCommand(*commandLine, accept);
}
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && out/build/clangcl-debug/bin/contour_gui_test.exe "[picker]"`
Expected: `All tests passed` (11 test cases). Then the whole GUI suite, to prove the new `Q_PROPERTY` and signal disturb nothing: `out/build/clangcl-debug/bin/contour_gui_test.exe` — `All tests passed`. `clang-format -i src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp` (the five modified files).

- [ ] **Step 8: Commit**

```bash
git add src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp \
        src/contour/session/TerminalSessionManager_test.cpp \
        src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp
git commit -F - <<'EOF'
session: snapshot every session's commands for the recent-commands picker

The manager copies each open session's finished commands under that
terminal's lock, labelled with its tab, and the window controller ranks them
for the session the picker opened over, pins that session and inserts the
accepted row into it.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.9: The `OpenRecentCommands` action

**Files:**
- Modify: `src/contour/config/Actions.hpp` (struct after `OpenCommandPalette` :71; variant after `OpenCommandPalette,` :178; `NonRepeatableActionConcept` :280-301; documentation after `documentation::OpenCommandPalette` :417-421; catalog row after the `OpenCommandPalette` row :683-684), `src/contour/session/TerminalSession.hpp:755`, `src/contour/session/TerminalSession.cpp:3399-3404`, `src/contour/config/Actions_test.cpp`, `src/contour/command/CommandCatalog_test.cpp`, `src/contour/session/TerminalSessionManager_test.cpp`

Phase 5 adds actions to the same variant; insert every piece **next to `OpenCommandPalette`** (by anchor, not by line number), so the catalog and the variant stay in the same order — `Actions_test.cpp:211` ("every catalog row sits at its own variant index") checks exactly that.

**Interfaces:**
- Consumes: `TerminalSessionManager::openRecentCommands` (Task 9.8).
- Produces (C8): `struct OpenRecentCommands {};` in `contour::actions`, an `Action` alternative with catalog name `"OpenRecentCommands"`, member of `NonRepeatableActionConcept`; `bool TerminalSession::operator()(actions::OpenRecentCommands);`.

`TerminalSession::executeAction` dispatches with `visit(*this, action)` (`TerminalSession.cpp:3657-3661`), so the alternative and its handler must land in the same commit or nothing compiles.

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/config/Actions_test.cpp`:

```cpp
TEST_CASE("actions: OpenRecentCommands is named, bare, documented and fires once per keypress",
          "[actions][catalog][picker]")
{
    using namespace contour::actions;

    auto const parsed = fromString("OpenRecentCommands");
    REQUIRE(parsed.has_value());
    CHECK(std::holds_alternative<OpenRecentCommands>(*parsed));
    CHECK(name(Action { OpenRecentCommands {} }) == "OpenRecentCommands");
    CHECK_FALSE(describe(Action { OpenRecentCommands {} }).empty());

    // It carries no argument, so the palette offers it straight from the catalog.
    CHECK_FALSE(isParameterized(Action { OpenRecentCommands {} }));

    // A one-shot UI gesture like OpenCommandPalette: by the second repeat the picker holds the keyboard.
    CHECK(isNonRepeatable(Action { OpenRecentCommands {} }));
}
```

Append to `src/contour/command/CommandCatalog_test.cpp`:

```cpp
TEST_CASE("The action palette offers the recent-commands picker", "[contour][palette][picker]")
{
    // Spec §9's "Open Recent Commands" palette entry: the catalog row is what puts it there, its title
    // derived from the action name like every other command's (README Phase 9 amendment).
    auto const commands = ActionCommandSource {}.commands();
    auto const found = std::ranges::find_if(commands, [](Command const& c) { return c.id == "OpenRecentCommands"; });

    REQUIRE(found != commands.end());
    CHECK(found->title == "Open Recent Commands");
}
```

Append to `src/contour/session/TerminalSessionManager_test.cpp` (add `#include <contour/config/Actions.hpp>` and `#include <QtTest/QSignalSpy>` to its includes):

```cpp
TEST_CASE("TerminalSessionManager: the OpenRecentCommands action opens the picker over its window",
          "[manager][picker]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* session = app.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    contour::test::runShellCommand(*session, "ls", 0);
    contour::test::showShellPrompt(*session);

    QSignalSpy const requested(win.controller, &contour::window::WindowController::recentCommandsRequested);
    CHECK(session->executeAction(contour::actions::OpenRecentCommands {}));

    CHECK(requested.count() == 1);
    CHECK(win->recentCommands()->rowCount() == 1);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'OpenRecentCommands': is not a member of 'contour::actions'`.

- [ ] **Step 3: Add the action**

In `src/contour/config/Actions.hpp`:

after `struct OpenCommandPalette{};` add

```cpp
struct OpenRecentCommands{}; // the picker over every open session's finished shell commands (spec §9)
```

in `using Action = std::variant<...>`, after `OpenCommandPalette,` add

```cpp
                            OpenRecentCommands,
```

in `NonRepeatableActionConcept`, after `OpenCommandPalette,` add

```cpp
                                                 OpenRecentCommands,
```

and extend the concept's comment sentence "Opening the command palette joins them because it is a one-shot UI gesture" to read "Opening the command palette or the recent-commands picker joins them because each is a one-shot UI gesture";

in `namespace documentation`, after the `OpenCommandPalette` constant add

```cpp
    constexpr inline std::string_view OpenRecentCommands {
        "Opens the recent-commands picker: the finished shell commands of every open tab, the current "
        "tab's first. Enter inserts the picked command at the prompt, Shift+Enter inserts and runs it; "
        "nothing is inserted unless the shell is at its prompt. Requires a shell that reports its "
        "commands (see Contour's shell integration)."
    };
```

and in `actionCatalog()`, after the `OpenCommandPalette` row add

```cpp
        ActionCatalogEntry {
            "OpenRecentCommands", Action { OpenRecentCommands {} }, documentation::OpenRecentCommands },
```

In `src/contour/session/TerminalSession.hpp`, after `bool operator()(actions::OpenCommandPalette);` add

```cpp
    bool operator()(actions::OpenRecentCommands);
```

In `src/contour/session/TerminalSession.cpp`, after `TerminalSession::operator()(actions::OpenCommandPalette)` add

```cpp

bool TerminalSession::operator()(actions::OpenRecentCommands)
{
    // Window-scoped like the palette: the manager routes it to the window this session is in, and the
    // picker inserts into THIS session.
    _manager->openRecentCommands(/*acting*/ this);
    return true;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test && out/build/clangcl-debug/bin/contour_test.exe "[actions],[palette],[picker]" && out/build/clangcl-debug/bin/contour_gui_test.exe "[picker]"`
Expected: both `All tests passed`, including `actions: every catalog row sits at its own variant index` and `actions: every action in the catalog round-trips through its name`. `clang-format -i src/contour/config/Actions.hpp src/contour/config/Actions_test.cpp src/contour/command/CommandCatalog_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager_test.cpp` (the six C++ files touched).

- [ ] **Step 5: Commit**

```bash
git add src/contour/config/Actions.hpp src/contour/config/Actions_test.cpp \
        src/contour/command/CommandCatalog_test.cpp src/contour/session/TerminalSession.hpp \
        src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager_test.cpp
git commit -F - <<'EOF'
actions: add OpenRecentCommands

A catalog action, so it is bindable, documented and offered by the action
palette as "Open Recent Commands"; it opens the recent-commands picker over
the acting session's window and is dropped on key auto-repeat like the
palette.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.10: Bind `Ctrl+Alt+R` as a built-in fallback

**Files:**
- Modify: `src/contour/config/Config.cpp` (`builtinFallbackCharMappings()`, Task 5.17: after its `Ctrl+Shift+G` row), `src/contour/config/Config_test.cpp`, `docs/configuration/key-mapping.md` (built-in bindings table, after Task 5.17's `Ctrl+Shift+G` row)

**Interfaces:**
- Consumes: `actions::OpenRecentCommands` (Task 9.9); `FallbackCharMapping`, `builtinFallbackCharMappings()` and the `char32_t` `applyBuiltinFallback` overload (Task 5.17), which `TerminalSession::sendCharEvent` already consults after the user's own character bindings.
- Produces: one `FallbackCharMapping` row in `builtinFallbackCharMappings()`: modifiers `Control | Alt`, input `U'R'`, binding `OpenRecentCommands`. `defaultInputMappings()` is not touched: a new default never reaches a user whose generated contour.yml already lists `input_mapping:` (`Config.hpp:172-178`, phase 5 Decision 6).

- [ ] **Step 1: Verify the chord is free on every platform (spec §16.1)**

Run:

```bash
sed -n '/inline InputMappings const& defaultInputMappings()/,/^}$/p' src/contour/config/Config.hpp | grep -nE "#if|#ifdef|input = U?'[Rr]'"
grep -nE "input = U?'[Rr]'|Key::R\b" src/contour/config/Config.cpp
```

Expected: **no output from either** (both `grep`s exit 1). The first proves that the default table has no platform branch (so one table answers for Linux, macOS, Windows and the BSDs) and binds no `R` under any modifier; the second that no fallback table — `builtinFallbackKeyMappings()`, `builtinFallbackMouseMappings()` or Task 5.17's `builtinFallbackCharMappings()`, whose only row is `Ctrl+Shift+G`'s `U'G'` — binds R. The existing `Ctrl+Alt` chords are `,` `V` `S` `K` `J` `O` `.` (`Config.hpp:946-1163`). If either command prints a line, `Ctrl+Alt+R` is taken: stop, bind `Ctrl+Shift+Alt+R` instead in Step 4 and in every test below, and record the substitution in the commit body.

- [ ] **Step 2: Write the failing tests**

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: Ctrl+Alt+R opens the recent-commands picker by default", "[config][picker]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    auto const config = contour::config::Config {};
    auto const ctrlAlt = Modifiers { Modifier::Control, Modifier::Alt };

    auto const* bound = contour::config::applyBuiltinFallback(config, U'R', ctrlAlt, uint8_t { 0 });
    REQUIRE(bound != nullptr);
    CHECK(std::holds_alternative<contour::actions::OpenRecentCommands>(bound->at(0)));

    // Exactly the chord: not Ctrl+R, which the shell's own history search owns.
    CHECK(contour::config::applyBuiltinFallback(config, U'R', Modifiers { Modifier::Control }, uint8_t { 0 })
          == nullptr);

    // A fallback, not a default -- it must reach users whose contour.yml lists the defaults -- and no
    // default claims the chord first (spec §16.1: one table, no platform branches, answers for all).
    CHECK(std::ranges::none_of(config.inputMappings.value().charMappings, [&](auto const& mapping) {
        return (mapping.input == U'R' || mapping.input == U'r') && mapping.modifiers == ctrlAlt;
    }));
}

TEST_CASE("Config: OpenRecentCommands binds from input_mapping", "[config][picker]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Alt], key: 'R', action: OpenRecentCommands }
)"sv);

    // An uppercase letter key may land in either list (see the SetTabTitle case above).
    auto const& mappings = config.inputMappings.value();
    auto const isPicker = [](auto const& mapping) {
        return !mapping.binding.empty()
               && std::holds_alternative<contour::actions::OpenRecentCommands>(mapping.binding.at(0));
    };
    CHECK((std::ranges::any_of(mappings.keyMappings, isPicker)
           || std::ranges::any_of(mappings.charMappings, isPicker)));
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && out/build/clangcl-debug/bin/contour_gui_test.exe "[config][picker]"`
Expected: FAIL — `Ctrl+Alt+R opens the recent-commands picker by default`: `REQUIRE( bound != nullptr )` fails, since no fallback row binds R yet. (`OpenRecentCommands binds from input_mapping` already passes: Task 9.9 made the action loadable.)

- [ ] **Step 4: Add the fallback row**

In `src/contour/config/Config.cpp`, in `builtinFallbackCharMappings()`, right after the `Ctrl+Shift+G` row (the entry ending `.binding = { { actions::OpenCommandOutput {} } } } },`) insert:

```cpp
        // Ctrl+Alt+R: the recent-commands picker (VS Code's chord for the same thing). Checked to be bound
        // nowhere in the defaults, on any platform, and by no other fallback row when it was added (spec
        // §16.1). A character row for the same reason as Ctrl+Shift+G's: the chord arrives as one.
        FallbackCharMapping { .mapping = { .modes { vtbackend::MatchModes {} },
                                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control,
                                                                               vtbackend::Modifier::Alt } },
                                           .input = U'R',
                                           .binding = { { actions::OpenRecentCommands {} } } } },
```

The row deliberately has no `AlternateScreen` mode filter (unlike `ScrollMarkUp`): spec §9 wants the picker to open in vim or less and say "not at a shell prompt" rather than pass the chord through. A user's own binding of `Ctrl+Alt+R` still wins: `sendCharEvent` consults the fallback only when the user's character mappings claim nothing (Task 5.17).

In `docs/configuration/key-mapping.md`, add to the built-in bindings table after Task 5.17's `Ctrl+Shift+G` row:

```markdown
| `Ctrl+Alt+R` | Open the recent-commands picker (`OpenRecentCommands`; the commands of every open tab) |
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test && out/build/clangcl-debug/bin/contour_test.exe "[palette],[picker]" && out/build/clangcl-debug/bin/contour_gui_test.exe "[config],[picker]"`
Expected: both `All tests passed` — Task 5.17's `Ctrl+Shift+G opens the last command's output by default` included. `clang-format -i src/contour/config/Config.cpp src/contour/config/Config_test.cpp`.

- [ ] **Step 6: Commit**

```bash
git add src/contour/config/Config.cpp src/contour/config/Config_test.cpp docs/configuration/key-mapping.md
git commit -F - <<'EOF'
config: open the recent-commands picker with Ctrl+Alt+R

VS Code's chord, verified free: defaultInputMappings() has no platform
branches and binds no R, and no fallback table binds R. A built-in
fallback row rather than a default, so it reaches users whose contour.yml
already lists input_mapping, and a user binding of the same chord still
wins. Listed in the built-in bindings table.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.11: One popup, two row sources — generalise `CommandPalette.qml`

**Files:**
- Modify: `src/contour/qml/CommandPalette.qml` (whole file)
- Create: `src/contour/test/RecentCommandsPickerQml_test.cpp`
- Modify: `src/contour/CMakeLists.txt` (`contour_gui_test` sources)

**Interfaces:**
- Consumes: `WindowController.commandPalette` + `runCommand(id)` (unchanged); `WindowController.recentCommands` + `acceptRecentCommand(row, how)` (Task 9.8); `RecentCommandsModel` roles `command`, `commandMatches`, `exitStatus`, `directory`, `age`, `tabName`, `runCount` and properties `filter`, `notice` (Task 9.6).
- Produces (QML): `enum Mode { Actions, RecentCommands }`, `enum Accept { Insert, Run }`, `property int mode` (default `CommandPalette.Mode.Actions`), `function acceptCurrent()` (unchanged signature), `function acceptCurrentAndRun()`; object names `commandPaletteNotice`, `recentCommandText`, `recentCommandStatus`, `recentCommandAge`, `recentCommandDetail` (the existing `commandPaletteFilter`, `commandPaletteList`, `commandPaletteTitle`, `commandPaletteShortcut`, `commandPaletteDescription` keep their meaning).

What stays identical in action mode, and is pinned by the **unedited** tests in `QmlComponents_test.cpp:2829-3510` and `CommandPaletteModel_test.cpp`: the rows and their delegate (moved verbatim into `Component { id: commandRow }`), `acceptCurrent()` taking no argument, the close-then-`Qt.callLater` dispatch, the focus hand-back, Ctrl+J/Ctrl+K, and the highlight rendering. The only behavioural addition is Shift+Enter, which in action mode runs the command exactly as Enter does.

- [ ] **Step 1: Write the failing tests**

Create `src/contour/test/RecentCommandsPickerQml_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The recent-commands picker: CommandPalette.qml in its RecentCommands mode, driven offscreen against a
// REAL RecentCommandsModel, so the delegate binds the roles production publishes. The action mode keeps
// its own tests in QmlComponents_test.cpp, unedited: the row-source seam must not change it.

#include <contour/command/RecentCommands.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/window/RecentCommandsModel.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QTest>

using namespace std::chrono_literals;
using contour::command::PromptReadiness;
using contour::command::RecentCommandsSnapshot;

namespace
{
constexpr auto Epoch = std::chrono::system_clock::time_point { std::chrono::seconds { 1'700'000'000 } };

/// A stand-in for WindowController exposing exactly the recent-commands surface CommandPalette.qml binds
/// in that mode: the model, and acceptRecentCommand(), whose calls it records.
class MockRecentController: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* recentCommands READ recentCommands CONSTANT)

  public:
    MockRecentController():
        _model { std::make_unique<contour::window::RecentCommandsModel>(_wall, std::string { "/home/user" }) }
    {
    }

    [[nodiscard]] QObject* recentCommands() const noexcept { return _model.get(); }
    [[nodiscard]] contour::window::RecentCommandsModel& model() noexcept { return *_model; }

    /// Fills the model the way WindowController::openRecentCommands() does.
    void open(RecentCommandsSnapshot snapshot) { _model->refresh(std::move(snapshot)); }

    /// Records what the picker asked to insert.
    Q_INVOKABLE void acceptRecentCommand(int row, int how) { accepted.emplace_back(row, how); }
    std::vector<std::pair<int, int>> accepted; //!< (row, how) per accept, in order.

  private:
    core::platform::ManualWallClock _wall { Epoch + 600s };
    std::unique_ptr<contour::window::RecentCommandsModel> _model;
};

/// "make test" and "git status" here, "cargo build" in the tab "build" — listed in that order.
[[nodiscard]] RecentCommandsSnapshot snapshotWith(PromptReadiness readiness)
{
    auto const row = [](std::string commandLine, uint64_t sessionId, std::chrono::seconds at, std::string tab) {
        return contour::command::RecentCommandRow { .commandLine = std::move(commandLine),
                                                    .exitCode = 0,
                                                    .workingDirectory = "/home/user/src",
                                                    .finishedAt = Epoch + at,
                                                    .sessionId = sessionId,
                                                    .tabName = std::move(tab) };
    };
    return RecentCommandsSnapshot {
        .rows = { row("make test", 1, 400s, "zsh"), row("git status", 1, 300s, "zsh"), row("cargo build", 2, 500s, "build") },
        .currentSessionId = 1,
        .readiness = readiness,
    };
}

/// Hosts the picker in a Window that, like Main.qml's, declares restoreTerminalFocus() and counts it.
struct PickerHost
{
    std::unique_ptr<QObject> window; //!< The wrapper Window.
    QObject* picker = nullptr;       //!< The CommandPalette inside it (owned by the window).

    [[nodiscard]] int focusRestores() const { return window->property("focusRestores").toInt(); }
};

/// Builds the host, opens the picker and pumps the event loop so the popup materializes its content.
[[nodiscard]] PickerHost openPicker(QQmlEngine& engine, MockRecentController& controller)
{
    contour::test::installChromeStyle(engine);
    engine.rootContext()->setContextProperty("recentController", &controller);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import Contour.Ui\n"
                                        "Window {\n"
                                        "  id: host\n"
                                        "  width: 800; height: 600; visible: true\n"
                                        "  property int focusRestores: 0\n"
                                        "  property alias pickerItem: picker\n"
                                        "  function restoreTerminalFocus() { host.focusRestores++; }\n"
                                        "  CommandPalette {\n"
                                        "    id: picker\n"
                                        "    mode: CommandPalette.Mode.RecentCommands\n"
                                        "    controller: recentController\n"
                                        "    window: host\n"
                                        "  }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/RecentPickerTestHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();

    auto host = PickerHost {};
    if (!component.isReady())
    {
        UNSCOPED_INFO("RecentPickerTestHost errors: " << component.errorString().toStdString());
        return host;
    }

    host.window.reset(component.create());
    if (host.window == nullptr)
        return host;

    host.picker = host.window->property("pickerItem").value<QObject*>();
    if (host.picker == nullptr)
        return host;

    QMetaObject::invokeMethod(host.picker, "open");
    QCoreApplication::processEvents();
    return host;
}

/// The delegate item for @p index of @p list, laid out.
[[nodiscard]] QQuickItem* rowItemAt(QQuickItem* list, int index)
{
    QMetaObject::invokeMethod(list, "forceLayout");
    QCoreApplication::processEvents();
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index));
    return item;
}

/// The `text` of the child named @p objectName under @p item, or empty.
[[nodiscard]] QString childText(QQuickItem* item, QString const& objectName)
{
    auto const* child = item != nullptr ? item->findChild<QQuickItem*>(objectName) : nullptr;
    return child != nullptr ? child->property("text").toString() : QString {};
}
} // namespace

TEST_CASE("The recent-commands picker lists the model's rows (offscreen)", "[contour][gui][qml][picker]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockRecentController controller;
    controller.open(snapshotWith(PromptReadiness::AtPrompt));

    auto const host = openPicker(engine, controller);
    REQUIRE(host.picker != nullptr);

    auto* list = host.picker->findChild<QQuickItem*>(QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);
    CHECK(list->property("count").toInt() == 3);

    auto* filter = host.picker->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    CHECK(filter->property("placeholderText").toString() == QStringLiteral("Type to search recent commands…"));

    SECTION("another tab's command shows its directory and its tab")
    {
        auto* item = rowItemAt(list, 2);
        REQUIRE(item != nullptr);
        CHECK(childText(item, QStringLiteral("recentCommandText")).contains(QStringLiteral("cargo build")));
        CHECK(childText(item, QStringLiteral("recentCommandDetail")) == QStringLiteral("~/src  ·  build"));
        CHECK(childText(item, QStringLiteral("recentCommandAge")) == QStringLiteral("1m ago"));
        CHECK(childText(item, QStringLiteral("recentCommandStatus")) == QStringLiteral("✓"));
    }

    SECTION("this tab's command shows no tab name")
    {
        CHECK(childText(rowItemAt(list, 0), QStringLiteral("recentCommandDetail")) == QStringLiteral("~/src"));
    }

    SECTION("nothing to explain: the notice is collapsed")
    {
        auto* notice = host.picker->findChild<QQuickItem*>(QStringLiteral("commandPaletteNotice"));
        REQUIRE(notice != nullptr);
        CHECK_FALSE(notice->isVisible());
    }

    SECTION("typing filters through the model and bolds the matched characters")
    {
        filter->setProperty("text", QStringLiteral("cb"));
        QCoreApplication::processEvents();
        CHECK(controller.model().filter() == QStringLiteral("cb"));
        REQUIRE(list->property("count").toInt() == 1);
        CHECK(childText(rowItemAt(list, 0), QStringLiteral("recentCommandText")).contains(QStringLiteral("<b>")));
    }

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Enter inserts the highlighted recent command, Shift+Enter also runs it (offscreen)",
          "[contour][gui][qml][picker]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockRecentController controller;
    controller.open(snapshotWith(PromptReadiness::AtPrompt));

    auto const host = openPicker(engine, controller);
    REQUIRE(host.picker != nullptr);
    auto* filter = host.picker->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    QMetaObject::invokeMethod(filter, "forceActiveFocus");
    QCoreApplication::processEvents();
    auto* const keyWindow = filter->window();
    REQUIRE(keyWindow != nullptr);

    SECTION("Enter inserts row 0, after the picker has closed and handed the keyboard back")
    {
        QTest::keyClick(keyWindow, Qt::Key_Return);
        QCoreApplication::processEvents(); // the deferred dispatch

        REQUIRE(controller.accepted.size() == 1);
        CHECK(controller.accepted.front() == std::pair { 0, 0 });
        CHECK_FALSE(host.picker->property("visible").toBool());
        CHECK(host.focusRestores() == 1);
    }

    SECTION("Shift+Enter inserts and runs")
    {
        QTest::keyClick(keyWindow, Qt::Key_Return, Qt::ShiftModifier);
        QCoreApplication::processEvents();

        REQUIRE(controller.accepted.size() == 1);
        CHECK(controller.accepted.front() == std::pair { 0, 1 });
    }

    SECTION("the row the user moved to is the one accepted")
    {
        QTest::keyClick(keyWindow, Qt::Key_Down);
        QTest::keyClick(keyWindow, Qt::Key_Return);
        QCoreApplication::processEvents();

        REQUIRE(controller.accepted.size() == 1);
        CHECK(controller.accepted.front() == std::pair { 1, 0 });
    }

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("The picker says when it cannot insert and when it has nothing to list (offscreen)",
          "[contour][gui][qml][picker]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockRecentController controller;

    SECTION("not at a shell prompt: the rows stay, the notice says why nothing will be inserted")
    {
        controller.open(snapshotWith(PromptReadiness::NotAtPrompt));
        auto const host = openPicker(engine, controller);
        REQUIRE(host.picker != nullptr);

        auto* notice = host.picker->findChild<QQuickItem*>(QStringLiteral("commandPaletteNotice"));
        REQUIRE(notice != nullptr);
        CHECK(notice->isVisible());
        CHECK(notice->property("text").toString().contains(QStringLiteral("Not at a shell prompt")));
        auto* list = host.picker->findChild<QQuickItem*>(QStringLiteral("commandPaletteList"));
        REQUIRE(list != nullptr);
        CHECK(list->property("count").toInt() == 3);
    }

    SECTION("no shell integration: an empty list with the hint, and Enter accepts nothing (review focus #5)")
    {
        controller.open(RecentCommandsSnapshot {});
        auto const host = openPicker(engine, controller);
        REQUIRE(host.picker != nullptr);

        auto* notice = host.picker->findChild<QQuickItem*>(QStringLiteral("commandPaletteNotice"));
        REQUIRE(notice != nullptr);
        CHECK(notice->isVisible());
        CHECK(notice->property("text").toString().contains(QStringLiteral("contour generate integration")));

        QMetaObject::invokeMethod(host.picker, "acceptCurrent");
        QCoreApplication::processEvents();
        CHECK(controller.accepted.empty());
    }

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

#include <RecentCommandsPickerQml_test.moc>
```

Register it: in `src/contour/CMakeLists.txt`, in `add_executable(contour_gui_test ...)`, add after `test/QmlComponents_test.cpp`:

```cmake
            test/RecentCommandsPickerQml_test.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && out/build/clangcl-debug/bin/contour_gui_test.exe "[qml][picker]"`
Expected: FAIL — `REQUIRE( host.picker != nullptr )`, with an info line `RecentPickerTestHost errors: ...` naming the `mode` property / `CommandPalette.Mode` that the current file does not have.

- [ ] **Step 3: Replace `src/contour/qml/CommandPalette.qml`**

```qml
// vim:syntax=qml
// The command palette's popup: a searchable list over one row source.
//
// One popup, two lists — `mode`:
//
//   - Actions (the default): every runnable command. Opened by the OpenCommandPalette action
//     (Ctrl+Shift+P by default), which routes through the session manager to this window's
//     WindowController, whose `commandPaletteRequested` signal Main.qml connects to open(). The rows come
//     from the controller's CommandPaletteModel: with an empty filter the list is sectioned — RECENTLY
//     USED (persisted across restarts) on top, then ALL COMMANDS alphabetically; as soon as the user
//     types, the model collapses both into one fuzzy-ranked list and clears `sectioned`, so the headers
//     disappear on their own without this file deciding when. Each row shows the command's title, its
//     key binding right-aligned (so the user learns the shortcut for next time), and its documentation
//     beneath.
//   - RecentCommands: every open session's finished shell commands, from the controller's
//     RecentCommandsModel (OpenRecentCommands, Ctrl+Alt+R built in; `recentCommandsRequested`). Enter
//     inserts the highlighted command at the prompt, Shift+Enter inserts and runs it. The model's
//     `notice` says when there is nothing to list (no shell integration) or nowhere to insert (not at a
//     shell prompt).
//
// Everything the two lists differ in is one row of the `modes` table below: where the rows come from, how
// one is drawn, which delegate property names the accepted row, the filter's placeholder, whether the
// source carries a notice, and what accepting a row does. The popup itself — filter box, keyboard
// handling, focus hand-back, deferred dispatch — is shared, so a third list is a third row.
import QtQuick
import QtQuick.Controls

Popup {
    id: root

    // The lists this popup can show (see the header comment). The order matches the `modes` table.
    enum Mode { Actions, RecentCommands }
    // How a row was accepted: Enter inserts, Shift+Enter also runs. Mirrors command::RecentCommandAccept,
    // whose values WindowController::acceptRecentCommand() receives. An action runs either way.
    enum Accept { Insert, Run }

    // The WindowController. Null-guarded EVERYWHERE below: the controller is destroyed before the QML
    // tree on window close, and QML re-evaluates dependent bindings once more against null during that
    // teardown. An unguarded `controller.` would raise a TypeError — which the run-wide QML message
    // gate in test_main.cpp turns into a failure of the whole test suite. (Same reason TabStrip.qml
    // guards its bindings.)
    required property var controller
    // The ApplicationWindow, so closing the palette can hand keyboard focus back to the terminal.
    required property var window
    // Which list this instance shows. Fixed per instance: Main.qml declares one of each.
    property int mode: CommandPalette.Mode.Actions

    // {{{ The row-source seam: one row per Mode, in the enum's order.
    readonly property var modes: [
        {
            // Mode.Actions
            source: "commandPalette",
            delegate: commandRow,
            keyRole: "commandId",
            placeholder: qsTr("Type to search commands…"),
            hasNotice: false,
            accept: function (target, key, how) { target.runCommand(key); }
        },
        {
            // Mode.RecentCommands
            source: "recentCommands",
            delegate: recentCommandRow,
            keyRole: "index",
            placeholder: qsTr("Type to search recent commands…"),
            hasNotice: true,
            accept: function (target, key, how) { target.acceptRecentCommand(key, how); }
        }
    ]
    readonly property var spec: root.modes[root.mode]
    // The list model the rows come from, read off the controller by the table's property name.
    readonly property var source: root.controller ? root.controller[root.spec.source] : null
    // Why this list cannot simply be used, or empty. Only a source that carries a notice is asked.
    readonly property string notice: root.spec.hasNotice && root.source ? root.source.notice : ""
    // The recent-command status marks, indexed by command::RecentCommandStatus (Unknown, Succeeded, Failed).
    readonly property var statusMarks: ["•", "✓", "✗"]
    // }}}

    modal: true
    focus: true
    padding: 0

    // Centered, and sized to the window rather than to the content: a list whose width shifted with
    // the longest visible command would jump on every keystroke.
    anchors.centerIn: Overlay.overlay
    width: Math.min(720, root.window ? root.window.width * 0.9 : 720)
    height: Math.min(520, root.window ? root.window.height * 0.8 : 520)

    // Live OS palette handle so the popup follows dark/light in realtime (see TabColorFlyout).
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    background: PopupSurface {}

    // Refresh + focus on every open, not just the first: the model's rows are rebuilt by the
    // controller (open tabs change), and the filter must start empty with the caret already in it so
    // the user can type immediately.
    onOpened: {
        filterField.clear();
        filterField.forceActiveFocus();
        list.currentIndex = list.count > 0 ? 0 : -1;
    }

    // The accepted row's key (its `keyRole`) and how it was accepted, held until this popup has actually
    // closed — see onClosed. A null key means nothing is pending.
    property var pendingKey: null
    property int pendingAccept: CommandPalette.Accept.Insert

    // Whatever closed the palette — Escape, a pick, a click outside — the terminal must get the keyboard
    // back, or the user is left typing into nothing. Only THEN does the picked row run: a command that
    // opens a keyboard-driven surface of its own (SetTabColor's swatch picker, SetTabTitle's rename field)
    // takes the focus as it opens, and running it any earlier means this restore takes that focus straight
    // back and the surface cannot be typed into at all. Same law, and the same shape, as
    // TabContextMenu.qml's colorPending: act once the popup is genuinely gone.
    //
    // Qt.callLater, not a direct call: "genuinely gone" is not yet true INSIDE this handler. Qt emits
    // closed() from within the popup's own exit transition, so a command that re-opens the palette —
    // OpenCommandPalette, a row this palette itself always offers — would re-enter Popup.open() on top of
    // the close that is still unwinding, and the palette would need dismissing twice. Deferring to the
    // next event-loop turn dispatches the command with the popup's state machine at rest.
    onClosed: {
        if (root.window)
            root.window.restoreTerminalFocus();
        const key = root.pendingKey;
        const how = root.pendingAccept;
        root.pendingKey = null; // cleared BEFORE dispatch: the command may re-open this palette
        if (key != null && root.controller)
            Qt.callLater(root.dispatch, key, how);
    }

    // The deferred half of onClosed: the mode's on-accept callback. Re-checks the controller: the window
    // can be torn down between the close and the event-loop turn that gets here.
    function dispatch(key, how) {
        if (root.controller)
            root.spec.accept(root.controller, key, how);
    }

    // Takes the highlighted row and closes; onClosed above is what runs it. Reads the key off the
    // highlighted DELEGATE (by the mode's `keyRole`) rather than indexing the model by role number, so
    // there is no magic 257 here to drift out of step with the models' Roles. A null currentItem (an
    // empty filtered list, or a row not yet realized) does nothing rather than accepting an arbitrary row.
    function acceptCurrent() {
        root.acceptCurrentAs(CommandPalette.Accept.Insert);
    }

    // Shift+Enter: as acceptCurrent(), and a recent command also runs.
    function acceptCurrentAndRun() {
        root.acceptCurrentAs(CommandPalette.Accept.Run);
    }

    function acceptCurrentAs(how) {
        if (!list.currentItem)
            return;
        root.pendingKey = list.currentItem[root.spec.keyRole];
        root.pendingAccept = how;
        root.close();
    }

    function acceptByKey(event) {
        if (event.modifiers & Qt.ShiftModifier)
            root.acceptCurrentAndRun();
        else
            root.acceptCurrent();
    }

    // Renders a title as StyledText with the filter's matched characters emphasized, so the user sees
    // WHY a row matched. `matches` is the ascending list of indices the model's fuzzy filter landed on
    // (empty when unfiltered). Matched characters are always bold; on an unselected row they also take
    // the accent colour, but on the SELECTED row — whose background is already that accent — bold alone
    // is used, since a tint there would wash out. The colour is chosen here, in the view, because only
    // QML knows the live OS accent and which row is current.
    //
    // The whole string is HTML-escaped (a live tab title or a command line can contain & < >), so it is
    // always valid StyledText. `matches` are UTF-16 code-unit indices (the models convert them from the
    // fuzzy filter's UTF-8 byte offsets), so they line up with charAt() even for multibyte text.
    function highlightedTitle(text, matches, selected) {
        var open = selected ? "<b>" : "<b><font color=\"" + systemPalette.highlight.toString() + "\">";
        var close = selected ? "</b>" : "</font></b>";
        var next = 0;
        var result = "";
        for (var i = 0; i < text.length; ++i) {
            var hit = matches && next < matches.length && matches[next] === i;
            if (hit) {
                result += open;
                ++next;
            }
            var c = text.charAt(i);
            if (c === "&")
                result += "&amp;";
            else if (c === "<")
                result += "&lt;";
            else if (c === ">")
                result += "&gt;";
            else
                result += c;
            if (hit)
                result += close;
        }
        return result;
    }

    contentItem: Column {
        spacing: 0

        // {{{ Filter box
        Item {
            id: filterRow
            width: parent.width
            height: filterField.height + 12

            TextField {
                id: filterField
                objectName: "commandPaletteFilter"
                x: 8
                y: 6
                width: parent.width - 16
                placeholderText: root.spec.placeholder
                // The model re-ranks on every keystroke; `sectioned` follows from the same write, so
                // the section headers vanish as soon as there is a query.
                onTextChanged: {
                    if (root.source)
                        root.source.filter = text;
                    list.currentIndex = list.count > 0 ? 0 : -1;
                }

                // Arrow keys move the SELECTION while the caret stays in the field, so the user never
                // has to leave the box to pick a row. Enter accepts (Shift+Enter also runs a recent
                // command), Escape dismisses.
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                Keys.onReturnPressed: (event) => root.acceptByKey(event)
                Keys.onEnterPressed: (event) => root.acceptByKey(event)
                Keys.onEscapePressed: root.close()

                // Vim-style navigation: Ctrl+J/Ctrl+K move the selection down/up, the same as the arrow
                // keys, so a home-row user never reaches for them. Accepted explicitly so Ctrl+J cannot
                // fall through as a line feed into the filter text. Other keys are left unaccepted here
                // and reach the named handlers above and normal text entry.
                Keys.onPressed: (event) => {
                    if (event.modifiers & Qt.ControlModifier) {
                        if (event.key === Qt.Key_J) {
                            list.incrementCurrentIndex();
                            event.accepted = true;
                        } else if (event.key === Qt.Key_K) {
                            list.decrementCurrentIndex();
                            event.accepted = true;
                        }
                    }
                }
            }
        }

        Rectangle {
            id: separator
            width: parent.width
            height: 1
            color: systemPalette.mid
        }
        // }}}

        // {{{ Notice — why the list cannot simply be used; collapsed (and skipped by the Column) when empty.
        Label {
            id: noticeLabel
            objectName: "commandPaletteNotice"
            visible: root.notice.length > 0
            width: parent.width
            leftPadding: 12
            rightPadding: 12
            topPadding: 6
            bottomPadding: 6
            text: root.notice
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.pixelSize: 11
            opacity: 0.8
            color: systemPalette.windowText
        }
        // }}}

        // {{{ The list
        ListView {
            id: list
            objectName: "commandPaletteList"
            width: parent.width
            // Takes whatever the filter row, its separator and the notice leave, so they cannot disagree
            // about the total and leave the last row clipped under the popup's bottom edge.
            height: root.height - filterRow.height - separator.height
                    - (noticeLabel.visible ? noticeLabel.height : 0)
            clip: true
            // Null-guarded: the controller dies before this tree on window close (see `controller`).
            model: root.source
            delegate: root.spec.delegate
            currentIndex: 0
            // Keeps the highlighted row on screen as the arrow keys walk past the viewport edge.
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}
        }
        // }}}
    }

    // {{{ Mode.Actions: a command — title + right-aligned shortcut, description beneath.
    Component {
        id: commandRow

        Item {
            id: row

            // Declared `required` so a missing role is a loud error rather than an `undefined`
            // silently rendering as an empty label.
            required property int index
            required property string commandId
            required property string title
            required property string description
            required property string shortcut
            required property int section
            required property bool sectionStart
            required property var titleMatches

            width: ListView.view.width
            height: header.height + entry.height

            // {{{ Section header — drawn by the FIRST row of each section.
            //
            // The model marks that row (`sectionStart`) rather than this delegate looking backwards
            // at its predecessor, which a virtualized ListView cannot reliably do. Collapses to zero
            // height when the list is filtered (the model reports one flat section then), so the
            // headers disappear without this file re-deriving "is there a query?".
            Item {
                id: header
                width: parent.width
                height: visible ? 24 : 0
                visible: row.sectionStart

                Label {
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.section === 0 ? qsTr("RECENTLY USED") : qsTr("ALL COMMANDS")
                    font.pixelSize: 10
                    font.bold: true
                    color: systemPalette.mid
                }
            }
            // }}}

            Rectangle {
                id: entry
                anchors.top: header.bottom
                width: parent.width
                height: 46
                color: row.index === list.currentIndex ? systemPalette.highlight : "transparent"

                property color textColor: row.index === list.currentIndex
                                          ? systemPalette.highlightedText
                                          : systemPalette.windowText

                Label {
                    id: titleLabel
                    objectName: "commandPaletteTitle"
                    x: 12
                    y: 5
                    // Yields to the shortcut rather than overrunning it: the shortcut is short and
                    // fixed, the title is the part that can be arbitrarily long.
                    width: parent.width - 24 - shortcutLabel.width - 8
                    // StyledText so the filter's matched characters can be bolded/accented; the raw
                    // title and its matched indices come from the model, the styling is chosen here.
                    // Re-evaluates when the selection moves, flipping the accent tint off the current
                    // row (see highlightedTitle).
                    textFormat: Text.StyledText
                    text: root.highlightedTitle(row.title, row.titleMatches,
                                                row.index === list.currentIndex)
                    elide: Text.ElideRight
                    color: entry.textColor
                }

                Label {
                    id: shortcutLabel
                    objectName: "commandPaletteShortcut"
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    y: 5
                    text: row.shortcut
                    font.family: "monospace"
                    font.pixelSize: 11
                    // Dimmed against the title: it is a hint for next time, not the thing being
                    // chosen now.
                    opacity: 0.75
                    color: entry.textColor
                }

                Label {
                    objectName: "commandPaletteDescription"
                    x: 12
                    anchors.top: titleLabel.bottom
                    anchors.topMargin: 1
                    width: parent.width - 24
                    text: row.description
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    opacity: 0.7
                    color: entry.textColor
                }

                HoverHandler {
                    // Hovering moves the selection, so the mouse and the keyboard agree on what
                    // "current" means and a click can never run a row other than the highlighted one.
                    onHoveredChanged: if (hovered) list.currentIndex = row.index
                }

                TapHandler {
                    onTapped: {
                        list.currentIndex = row.index;
                        root.acceptCurrent();
                    }
                }
            }
        }
    }
    // }}}

    // {{{ Mode.RecentCommands: a shell command — status mark, the command (matches emphasized), and
    // beneath it the directory it ran in plus, for another tab's command, that tab; right-aligned, how
    // often and how long ago it ran.
    Component {
        id: recentCommandRow

        Item {
            id: recentRow

            required property int index
            required property string command
            required property var commandMatches
            required property int exitStatus
            required property string directory
            required property string age
            required property string tabName
            required property int runCount

            readonly property bool isCurrent: recentRow.index === list.currentIndex
            readonly property color textColor: recentRow.isCurrent ? systemPalette.highlightedText
                                                                   : systemPalette.windowText

            width: ListView.view.width
            height: 46

            Rectangle {
                anchors.fill: parent
                color: recentRow.isCurrent ? systemPalette.highlight : "transparent"
            }

            Label {
                id: statusMark
                objectName: "recentCommandStatus"
                x: 12
                y: 5
                width: 14
                text: root.statusMarks[recentRow.exitStatus] || root.statusMarks[0]
                color: recentRow.textColor
            }

            Label {
                id: commandLabel
                objectName: "recentCommandText"
                x: 30
                y: 5
                // Yields to the age column, like a command title yields to its shortcut.
                width: parent.width - x - 12 - ageLabel.width - 8
                textFormat: Text.StyledText
                text: root.highlightedTitle(recentRow.command, recentRow.commandMatches, recentRow.isCurrent)
                elide: Text.ElideRight
                font.family: "monospace"
                color: recentRow.textColor
            }

            Label {
                id: ageLabel
                objectName: "recentCommandAge"
                anchors.right: parent.right
                anchors.rightMargin: 12
                y: 5
                textFormat: Text.PlainText
                text: recentRow.runCount > 1 ? qsTr("×%1 · %2").arg(recentRow.runCount).arg(recentRow.age)
                                             : recentRow.age
                font.pixelSize: 11
                opacity: 0.75
                color: recentRow.textColor
            }

            Label {
                objectName: "recentCommandDetail"
                x: 30
                anchors.top: commandLabel.bottom
                anchors.topMargin: 1
                width: parent.width - 42
                // Strings a program reported (a directory, a tab title): plain text only, so nothing in
                // them can be read as markup.
                textFormat: Text.PlainText
                text: recentRow.tabName.length > 0 ? recentRow.directory + "  ·  " + recentRow.tabName
                                                   : recentRow.directory
                font.pixelSize: 11
                elide: Text.ElideMiddle
                opacity: 0.7
                color: recentRow.textColor
            }

            HoverHandler {
                onHoveredChanged: if (hovered) list.currentIndex = recentRow.index
            }

            TapHandler {
                onTapped: {
                    list.currentIndex = recentRow.index;
                    root.acceptCurrent();
                }
            }
        }
    }
    // }}}
}
```

- [ ] **Step 4: Run the new tests and the unchanged palette tests**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && out/build/clangcl-debug/bin/contour_gui_test.exe "[qml][picker]" && out/build/clangcl-debug/bin/contour_gui_test.exe "[palette]" && out/build/clangcl-debug/bin/contour_gui_test.exe "[qml]"`
Expected: all three `All tests passed` — the recent-commands picker's 3 cases, every action-palette case in `QmlComponents_test.cpp` and `CommandPaletteModel_test.cpp` (not edited), and the load-every-component case (`QmlComponents_test.cpp:540-575`), which now loads the enum-bearing file. `clang-format -i src/contour/test/RecentCommandsPickerQml_test.cpp`.

- [ ] **Step 5: Commit**

```bash
git add src/contour/qml/CommandPalette.qml src/contour/test/RecentCommandsPickerQml_test.cpp src/contour/CMakeLists.txt
git commit -F - <<'EOF'
qml: serve the command palette and the recent-commands picker from one popup

CommandPalette.qml gains a row-source seam: a modes table naming each list's
model, delegate, key role, placeholder and on-accept callback, plus a notice
line. The action mode is unchanged; the recent-commands mode shows status,
command, directory, tab and age, inserts on Enter and runs on Shift+Enter.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.12: Wire the picker into `Main.qml`

**Files:**
- Modify: `src/contour/qml/Main.qml:222-230,262-268`, `src/contour/test/MainWindowQml_test.cpp` (mock :88-157,265; new test), `src/contour/session/KeyboardRouting_test.cpp` (mock :213-278,360)

**Interfaces:**
- Consumes: `WindowController.recentCommandsRequested` (Task 9.8); `CommandPalette.Mode.RecentCommands` (Task 9.11).
- Produces: a second `CommandPalette` instance with `objectName: "recentCommandsPicker"`; `Connections` handler `onRecentCommandsRequested`.

Both mock controllers that load `Main.qml` must carry the new surface: a `Connections` handler for a signal the target lacks is a QML warning, and the run-wide gate turns it into a suite failure — the reason the mocks already mirror `saveLayoutRequested` (`MainWindowQml_test.cpp:274-278`, `KeyboardRouting_test.cpp:371-374`).

- [ ] **Step 1: Write the failing test and update the mocks**

In `src/contour/test/MainWindowQml_test.cpp`, `MockMainController`: after `Q_PROPERTY(QObject* commandPalette READ commandPalette CONSTANT)` add

```cpp
    // Ditto for the recent-commands picker, a second CommandPalette instance in Main.qml: it reads
    // `recentCommands` and Connections-targets recentCommandsRequested.
    Q_PROPERTY(QObject* recentCommands READ recentCommands CONSTANT)
```

after `Q_INVOKABLE void openCommandPalette() { emit commandPaletteRequested(); }` add

```cpp
    [[nodiscard]] QObject* recentCommands() const noexcept { return nullptr; }
    Q_INVOKABLE void acceptRecentCommand(int /*row*/, int /*how*/) {}
    /// Mirrors WindowController::openRecentCommands(): what makes Main.qml's picker appear.
    Q_INVOKABLE void openRecentCommands() { emit recentCommandsRequested(); }
```

and after `void commandPaletteRequested();` in its `signals:` add

```cpp
    void recentCommandsRequested();
```

Then append the test before the trailing `#include <MainWindowQml_test.moc>`:

```cpp
TEST_CASE("Main.qml opens the recent-commands picker when its controller asks (offscreen)",
          "[contour][gui][qml][mainwindow][picker]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto* picker = root->findChild<QObject*>(QStringLiteral("recentCommandsPicker"));
    REQUIRE(picker != nullptr);
    CHECK(picker->property("mode").toInt() == 1); // CommandPalette.Mode.RecentCommands
    CHECK_FALSE(picker->property("visible").toBool());

    controller.openRecentCommands();
    QCoreApplication::processEvents();

    CHECK(picker->property("visible").toBool());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
```

In `src/contour/session/KeyboardRouting_test.cpp`, `RoutingMockController`, make the same three additions: the `recentCommands` `Q_PROPERTY` (with the same comment) after its `commandPalette` `Q_PROPERTY`, the `recentCommands()` / `acceptRecentCommand(int, int)` / `openRecentCommands()` members after its `openCommandPalette()`, and `void recentCommandsRequested();` after its `void commandPaletteRequested();`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && out/build/clangcl-debug/bin/contour_gui_test.exe "[mainwindow][picker]"`
Expected: FAIL — `REQUIRE( picker != nullptr )`.

- [ ] **Step 3: Add the instance and its handler to `Main.qml`**

In `src/contour/qml/Main.qml`, after the existing `CommandPalette { id: commandPalette ... }` block (ends line 230) insert:

```qml

    // The recent-commands picker (Ctrl+Alt+R): the same popup over a second row source — every open
    // session's finished shell commands — opened by the OpenRecentCommands action through THIS window's
    // controller, exactly like the palette above.
    CommandPalette {
        id: recentCommandsPicker
        objectName: "recentCommandsPicker"
        mode: CommandPalette.Mode.RecentCommands
        controller: appWindow.win
        window: appWindow
    }
```

and in the `Connections { target: appWindow.win ... }` block, after `function onCommandPaletteRequested() { commandPalette.open(); }` add

```qml
        function onRecentCommandsRequested() { recentCommandsPicker.open(); }
```

- [ ] **Step 4: Run the Main.qml suites**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && out/build/clangcl-debug/bin/contour_gui_test.exe "[mainwindow]" && out/build/clangcl-debug/bin/contour_gui_test.exe "[keyboard]" && out/build/clangcl-debug/bin/contour_gui_test.exe`
Expected: `All tests passed` each time — the new case, every existing Main.qml load case with zero QML diagnostics, the keyboard-routing cases, then the whole GUI suite (production `WindowController` and both mocks carry the surface Main.qml now binds). `clang-format -i src/contour/test/MainWindowQml_test.cpp src/contour/session/KeyboardRouting_test.cpp`.

- [ ] **Step 5: Commit**

```bash
git add src/contour/qml/Main.qml src/contour/test/MainWindowQml_test.cpp src/contour/session/KeyboardRouting_test.cpp
git commit -F - <<'EOF'
qml: open the recent-commands picker from Main.qml

A second CommandPalette instance in RecentCommands mode answers the window
controller's recentCommandsRequested; the Main.qml test mocks carry the new
surface so their loads stay diagnostic-free.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 9.13: Phase gate

**Files:** none new (fixes from review land in the files they concern).

- [ ] **Step 1: Full build, zero warnings, full suite**

Run: `cmake --build --preset clangcl-debug` (if `src/contour/display/*` fails on `yaml-cpp/emitter.h`, the pre-existing break the README notes, build the targets instead: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test vtbackend_test`), then `ctest --test-dir out/build/clangcl-debug --output-on-failure` and `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`.
Expected: no warnings in any file this phase touched; the ctest summary equals phase 0's baseline plus this phase's cases, with only the baseline's recorded environmental failures; spelling PASS (or SKIP as in the baseline). Record the `N tests passed, M failed` line.

- [ ] **Step 2: Hand-audit what clang-tidy cannot reach on Windows**

For every line this phase added under `src/contour/**` (`git diff <phase-9-start>..HEAD -- src/contour`): helpers in `.cpp` files sit in anonymous namespaces; unmodified locals are `const`; no widened `int * int`; no new `bool` parameter, return or member outside the QML-facing carve-outs (`isCurrent` is a QML delegate property); no `NOLINT`; no Windows-macro identifiers. Fix and amend nothing — fixes are a new commit in Step 4.

- [ ] **Step 3: `/simplify` over the phase**

Run `/simplify` on `git diff <phase-9-start>..HEAD`. Expected candidates to check: no second copy of the UTF-16 translation; no duplicated prompt-readiness expression (one `promptReadinessLocked()`); the test fixtures' `runShellCommand`/`showShellPrompt` used by both session test files rather than redefined; `runAndDeliverShellCommand` (6.7) may become `contour::test::runShellCommand` + `QCoreApplication::sendPostedEvents(&session, QEvent::MetaCall)`. Commit its fixes:

```bash
git add -u
git commit -F - <<'EOF'
picker: simplify after review of phase 9

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(Skip the commit when `/simplify` changes nothing.)

- [ ] **Step 4: Code review at xhigh**

Run `/code-review xhigh` on the phase's commits (or dispatch a review subagent with `effort: "xhigh"`). Points it must cover: the lock order (tab label resolved outside the terminal lock; prompt check and paste under one hold); the `QPointer` target; Review Focus #2 (alternate screen refuses, leaving restores), #3 (`ESC[201~` never reaches the shell, display sanitised), #5 (empty state with hint); action-mode palette behaviour unchanged. Fix every confirmed finding with its own test, then:

```bash
git add -u
git commit -F - <<'EOF'
picker: address phase 9 code review

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

and re-run Step 1. Put the final ctest summary line (`N tests passed, M failed out of T`) in this commit's body above the `Signed-off-by` line.

- [ ] **Step 5: Report to the coordinator**

Report the ctest summary, the spelling result, the review findings and their fixes. The coordinator records progress (never this session). Phase 10 starts from `HEAD`; it documents `OpenRecentCommands`/`Ctrl+Alt+R` on the website and in `metainfo.xml` (spec §15), which this phase deliberately leaves to it (Task 9.10 adds only the chord's row to `key-mapping.md`'s built-in bindings table, as Task 5.17 did for `Ctrl+Shift+G`).
