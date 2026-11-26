// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;

namespace
{

[[nodiscard]] std::vector<std::string> textsOf(std::vector<Row> const& rows, RowKind kind)
{
    auto texts = std::vector<std::string> {};
    for (auto const& row: rows)
        if (row.kind == kind)
            texts.push_back(row.text);
    return texts;
}

[[nodiscard]] Row const* rowOfRun(std::vector<Row> const& rows, std::string_view run)
{
    auto const found = std::ranges::find(rows, run, &Row::run);
    return found != rows.end() ? &*found : nullptr;
}

constexpr auto RunningId = "pr-2375-20261113-150847"sv;
constexpr auto DoneId = "pr-2379-20261113-150912"sv;
constexpr auto KeptId = "issue-2356-20261113-151001"sv;
constexpr auto SixMinutes = uint64_t { 360'000 };

} // namespace

TEST_CASE("the Fleet lists runs only, then + new task", "[orchestrator][view]")
{
    auto const engine = bootEngine({});
    auto const rows = buildRows(engine, StartMonoMs);
    // The pull requests and issues are the Work view's: with no run, the Fleet holds `+ new task` alone.
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().kind == RowKind::NewTask);
    CHECK(rows.front().isSelectable());
}

TEST_CASE("a folded group lists its heading, and its runs only under a filter", "[orchestrator][view]")
{
    auto const heading = [](Fold fold) {
        return Row { .kind = RowKind::Heading, .text = "FINISHED", .group = "fleet/finished", .fold = fold };
    };
    auto const run = [](std::string text, Fold fold) {
        return Row { .kind = RowKind::Finished,
                     .text = std::move(text),
                     .run = "r",
                     .group = "fleet/finished",
                     .fold = fold };
    };
    auto const open =
        std::vector { heading(Fold::Open), run("pr-1  fix CI", Fold::Open), run("pr-2  docs", Fold::Open) };
    auto const folded = std::vector { heading(Fold::Folded),
                                      run("pr-1  fix CI", Fold::Open),
                                      run("pr-2  docs", Fold::Folded) };

    // Open, the heading is no stop for the selection; folded, it is one, and the hidden run is left out.
    CHECK(visibleRows(open, "") == std::vector<std::size_t> { 0, 1, 2 });
    CHECK(selectableCount(open, visibleRows(open, "")) == 2);
    CHECK(visibleRows(folded, "") == std::vector<std::size_t> { 0, 1 });
    CHECK(selectableCount(folded, visibleRows(folded, "")) == 2);

    // A filter looks at the hidden runs too; a group none of whose runs holds it goes, heading and all.
    CHECK(visibleRows(folded, "docs") == std::vector<std::size_t> { 0, 2 });
    CHECK(visibleRows(folded, "nothing").empty());
}

TEST_CASE("an unconfigured engine says what to configure", "[orchestrator][view]")
{
    auto const engine = bootEngine({ .configJson = "{}" });
    auto const rows = buildRows(engine, StartMonoMs);
    REQUIRE(!rows.empty());
    CHECK(std::ranges::all_of(rows, [](Row const& row) { return row.kind == RowKind::Notice; }));
    CHECK(std::ranges::any_of(rows, [](Row const& row) { return row.text.contains("worktree_root"); }));
}

TEST_CASE("an agent waiting on its own work says what for, in Needs you", "[orchestrator][view]")
{
    auto waiting = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    waiting.lastStatus = AgentStatus::Idle;
    auto idle = storedRun(Running {}, DoneId, ChangeRequestRef { 2379 });
    idle.lastStatus = AgentStatus::Idle;
    auto const engine =
        bootEngine({ .stored = { waiting, idle },
                     .paneMessages = { { std::string { RunningId }, "Waiting for Build pace" },
                                       { std::string { DoneId }, "" } } });
    auto const rows = buildRows(engine, StartMonoMs + SixMinutes);

    auto const* const row = rowOfRun(rows, RunningId);
    REQUIRE(row != nullptr);
    CHECK(row->attention == FleetGroup::NeedsYou);
    CHECK(row->status == "waiting 6m");
    CHECK(row->context == "Waiting for Build pace");

    // Idle with nothing to say waits for an answer.
    auto const* const quiet = rowOfRun(rows, DoneId);
    REQUIRE(quiet != nullptr);
    CHECK(quiet->attention == FleetGroup::NeedsYou);
    CHECK(quiet->context == "waiting for your answer");
}

TEST_CASE("agent and finished rows", "[orchestrator][view]")
{
    auto kept = storedRun(Finished { .ended = Ended::EndedByYou,
                                     .fate = Fate::Kept,
                                     .entries = { " M src/a.cpp", "?? notes.txt" },
                                     .seen = Seen::New,
                                     .endedWallMs = DispatchWallMs },
                          KeptId,
                          IssueRef { 2356 },
                          "issue");
    auto const engine = bootEngine({ .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                                                 storedRun(Done {}, DoneId, ChangeRequestRef { 2379 }),
                                                 kept } });
    auto const rows = buildRows(engine, StartMonoMs + SixMinutes);
    auto const offers = [](Row const& row, std::initializer_list<KeyCommand> commands) {
        return std::ranges::all_of(
            commands, [&](KeyCommand command) { return std::ranges::contains(row.offers, command); });
    };

    auto const* const running = rowOfRun(rows, RunningId);
    REQUIRE(running != nullptr);
    CHECK(running->text == "pr-2375 · fix CI");
    CHECK(running->name == "pr-2375");
    CHECK(running->status == "working 6m");
    CHECK(running->attention == FleetGroup::Working);
    CHECK(offers(*running,
                 { KeyCommand::BoardTab,
                   KeyCommand::TranscriptTab,
                   KeyCommand::DetailsTab,
                   KeyCommand::EndRun,
                   KeyCommand::Reveal,
                   KeyCommand::OpenLink }));
    CHECK_FALSE(offers(*running, { KeyCommand::Dismiss }));

    auto const* const done = rowOfRun(rows, DoneId);
    REQUIRE(done != nullptr);
    CHECK(done->attention == FleetGroup::ToReview);
    CHECK(done->status == "done");
    CHECK(done->context == "reported done");
    CHECK(offers(*done, { KeyCommand::EndRun, KeyCommand::Reveal }));

    auto const* const finished = rowOfRun(rows, KeptId);
    REQUIRE(finished != nullptr);
    CHECK(finished->kind == RowKind::Finished);
    CHECK(finished->mark == "⚑");
    CHECK(finished->text == "issue-2356 · work on issue");
    CHECK(finished->status == "kept");
    CHECK(finished->context == "ended by you · 2 entries kept");
    CHECK(offers(
        *finished,
        { KeyCommand::BoardTab, KeyCommand::TranscriptTab, KeyCommand::RetryRemoval, KeyCommand::Dismiss }));
    CHECK_FALSE(offers(*finished, { KeyCommand::EndRun, KeyCommand::Reveal }));
    CHECK(std::ranges::contains(textsOf(rows, RowKind::Heading), "WORKING"));
    CHECK(std::ranges::contains(textsOf(rows, RowKind::Heading), "FINISHED"));
}

TEST_CASE("a run outside the configuration is marked and still endable", "[orchestrator][view]")
{
    constexpr auto RemovedId = "pr-2366-20261113-140211"sv;
    auto const finishedAs = [](Fate fate, std::string_view id, ItemRef item) {
        return outsideConfiguration(
            storedRun(Finished { .ended = Ended::EndedByYou,
                                 .fate = fate,
                                 .entries = fate == Fate::Kept ? std::vector<std::string> { "?? notes.txt" }
                                                               : std::vector<std::string> {},
                                 .seen = Seen::Opened,
                                 .endedWallMs = DispatchWallMs },
                      id,
                      std::move(item),
                      "issue"));
    };
    auto const engine = bootEngine(
        { .stored = { outsideConfiguration(storedRun(Running {}, RunningId, ChangeRequestRef { 2375 })),
                      finishedAs(Fate::Kept, KeptId, IssueRef { 2356 }),
                      finishedAs(Fate::Removed, RemovedId, ChangeRequestRef { 2366 }) } });
    auto const rows = buildRows(engine, StartMonoMs + SixMinutes);

    auto const outside =
        DetailLine { .label = "", .value = "outside the configuration", .role = Role::Warning };

    // Untouched, the row says where it belongs.
    auto const* const active = rowOfRun(rows, RunningId);
    REQUIRE(active != nullptr);
    CHECK(active->status == "outside the configuration");
    CHECK(std::ranges::contains(active->offers, KeyCommand::EndRun));
    CHECK(active->details.front() == outside);

    // Its outcome wins in the row; the details still say where it belongs. Its removal is never retried.
    for (auto const id: { KeptId, RemovedId })
    {
        CAPTURE(id);
        auto const* const finished = rowOfRun(rows, id);
        REQUIRE(finished != nullptr);
        CHECK(finished->kind == RowKind::Finished);
        CHECK(finished->details.front() == outside);
        CHECK_FALSE(std::ranges::contains(finished->offers, KeyCommand::RetryRemoval));
        // A removed run's board went with its worktree.
        CHECK(std::ranges::contains(finished->offers, KeyCommand::BoardTab) == (id == KeptId));
    }
    // The entries a kept one holds are still shown.
    CHECK(std::ranges::contains(rowOfRun(rows, KeptId)->details, "?? notes.txt", &DetailLine::value));
}

TEST_CASE("a run outside the configuration the user ended shows its progress", "[orchestrator][view]")
{
    auto ending = outsideConfiguration(storedRun(Ending { .step = EndStep::Status,
                                                          .cause = Ended::EndedByYou,
                                                          .resumedFrom = std::nullopt,
                                                          .removal = Removal::NotIssued },
                                                 RunningId,
                                                 ChangeRequestRef { 2375 }));
    auto const engine = bootEngine({ .stored = { ending } });
    auto const rows = buildRows(engine, StartMonoMs + SixMinutes);
    auto const* const row = rowOfRun(rows, RunningId);
    REQUIRE(row != nullptr);
    CHECK(row->status == "ending");
    CHECK(row->context == runStateText(engine.runs().front()));
    CHECK(row->details.front().value == "outside the configuration");
}

TEST_CASE("a refused configuration labels no run outside, and the trust hint still shows",
          "[orchestrator][view]")
{
    auto unreported = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    unreported.lastStatus = std::nullopt;
    auto const engine = bootEngine({ .configJson = R"({ "worktree_root": 7 })", .stored = { unreported } });
    REQUIRE(engine.config() == nullptr);
    REQUIRE(engine.scopeOf(engine.runs().front()) == RunScope::Unknown);

    auto const rows = buildRows(engine, StartMonoMs + 30'000);
    CHECK(rows.front().kind == RowKind::Notice);
    CHECK(std::ranges::contains(textsOf(rows, RowKind::Heading), "WORKING"));
    auto const* const row = rowOfRun(rows, RunningId);
    REQUIRE(row != nullptr);
    CHECK(row->status == "waiting? see its tab");
    CHECK(std::ranges::contains(row->offers, KeyCommand::EndRun));
    CHECK(std::ranges::none_of(row->details, [](DetailLine const& line) {
        return line.value.contains("outside the configuration");
    }));
    CHECK(nextRowChange(engine, StartMonoMs + 29'000) == StartMonoMs + 30'000);
}

TEST_CASE("with more than one repository each run names its own", "[orchestrator][view]")
{
    auto other = storedRun(Running {}, DoneId, ChangeRequestRef { 2379 });
    other.repo = "other";
    other.repoPath = std::string { OtherPath };
    auto const engine = bootEngine({
        .repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github", "issues": [] },
                      { "name": "other", "path": "~/projects/other", "forge": "github", "issues": [] } ])",
        .repoPaths = { std::string { ContourPath }, std::string { OtherPath } },
        .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }), other },
    });
    auto const rows = buildRows(engine, StartMonoMs);
    CHECK(rowOfRun(rows, RunningId)->prefix == "contour");
    CHECK(rowOfRun(rows, DoneId)->prefix == "other");
}

namespace
{

/// @return The index of the detail line labelled @p label, or the line count when there is none.
[[nodiscard]] std::size_t detailIndexOf(std::vector<DetailLine> const& details, std::string_view label)
{
    auto const found = std::ranges::find(details, label, &DetailLine::label);
    return static_cast<std::size_t>(std::ranges::distance(details.begin(), found));
}

/// @return The configuration's workers, as the `workers` array spells them.
[[nodiscard]] std::string workersJson(std::string_view workers)
{
    return std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "workers": [ {} ],
  "repos": [ {{ "name": "contour", "path": "~/projects/contour", "forge": "github", "issues": [] }} ],
  "tasks": [
    {{ "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
       "prompt": "/contour-workflows:fix-ci {{url}}" }},
    {{ "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
       "prompt": "/contour-workflows:work-issue {{url}}" }} ] }})",
                       workers);
}

} // namespace

TEST_CASE("a run names its worker beside its model when more than one worker is configured",
          "[orchestrator][view]")
{
    auto withWork = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    withWork.worker = "work";
    auto implicit = storedRun(Running {}, DoneId, ChangeRequestRef { 2379 });
    auto const engine = bootEngine({ .configJson = workersJson(R"({ "id": "work", "agent": "claude" },
                                                   { "id": "copilot", "agent": "copilot" })"),
                                     .stored = { withWork, implicit } });
    auto const rows = buildRows(engine, StartMonoMs);

    auto const* const named = rowOfRun(rows, RunningId);
    REQUIRE(named != nullptr);
    auto const modelAt = detailIndexOf(named->details, "model");
    REQUIRE(modelAt + 1 < named->details.size());
    CHECK(named->details[modelAt + 1] == DetailLine { .label = "worker", .value = "work" });

    // A run that stores no worker is the implicit one.
    auto const* const unnamed = rowOfRun(rows, DoneId);
    REQUIRE(unnamed != nullptr);
    auto const implicitAt = detailIndexOf(unnamed->details, "model");
    REQUIRE(implicitAt + 1 < unnamed->details.size());
    CHECK(unnamed->details[implicitAt + 1] == DetailLine { .label = "worker", .value = "claude" });
}

TEST_CASE("a single or no configured worker shows no worker line", "[orchestrator][view]")
{
    auto named = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    named.worker = "work";
    auto const single = bootEngine(
        { .configJson = workersJson(R"({ "id": "work", "agent": "claude" })"), .stored = { named } });
    auto const none = bootEngine({ .stored = { named } });

    for (auto const* const engine: { &single, &none })
    {
        auto const rows = buildRows(*engine, StartMonoMs);
        auto const* const row = rowOfRun(rows, RunningId);
        REQUIRE(row != nullptr);
        CHECK(detailIndexOf(row->details, "worker") == row->details.size());
    }
}

TEST_CASE("a filter keeps the runs that hold it, and their groups' headings", "[orchestrator][view]")
{
    auto const engine = bootEngine({ .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                                                 storedRun(Done {}, DoneId, ChangeRequestRef { 2379 }) } });
    auto const rows = buildRows(engine, StartMonoMs);
    auto const filtered = visibleRows(rows, "2379");
    // The run, its heading, and `+ new task`, which every filter keeps.
    CHECK(selectableCount(rows, filtered) == 2);
    CHECK(std::ranges::count_if(filtered,
                                [&](std::size_t index) { return rows[index].kind == RowKind::Heading; })
          == 1);
    CHECK(positionOfSelectable(rows, filtered, 0) < filtered.size());
    CHECK(positionOfSelectable(rows, filtered, 2) == filtered.size());
}

TEST_CASE("cut: text fits, or ends in an ellipsis within its cells", "[orchestrator][view]")
{
    CHECK(cut("short", 10) == "short");
    CHECK(cut("exactly10!", 10) == "exactly10!");
    CHECK(cut("one too long", 11) == "one too lo…");
    CHECK(cut("anything", 0).empty());
    CHECK(cut("a\x1b]52;c;ZXZpbA==\x07pwn", 40) == "a]52;c;ZXZpbA==pwn");
}

TEST_CASE("wide characters at the column edge", "[orchestrator][view]")
{
    auto const title = std::string { "語語語語語語語語語語" }; // 20 cells
    for (auto const columns: std::views::iota(1, 20))
    {
        CAPTURE(columns);
        auto const result = cut(title, columns);
        CHECK(columnsOf(result) <= columns);
        CHECK(result.ends_with("…"));
        // Only whole wide characters before the ellipsis: an odd width leaves one cell blank instead.
        auto const kept =
            std::string_view { result }.substr(0, result.size() - std::string_view { "…" }.size());
        CHECK(kept.size() % std::string_view { "語" }.size() == 0);
        CHECK(columnsOf(kept) == ((columns - 1) / 2) * 2);
    }
}

TEST_CASE("wrap: words fill each line; a word wider than a line is broken", "[orchestrator][view]")
{
    CHECK(wrap("git uses another directory", 10, 8)
          == std::vector<std::string> { "git uses", "another", "director", "y" });
    CHECK(wrap("short", 10, 8) == std::vector<std::string> { "short" });
    CHECK(wrap("a  b", 10, 8) == std::vector<std::string> { "a b" });
    CHECK(wrap("", 10, 8).empty());
    CHECK(wrap("anything", 0, 8).empty());
    CHECK(wrap("a\x1b]52;c;x\x07"
               "b c",
               40,
               40)
          == std::vector<std::string> { "a]52;c;xb c" });
    // Never half a wide character: an odd width leaves a cell blank.
    for (auto const& line: wrap("語語語語語", 5, 3))
        CHECK(columnsOf(line) <= 5);
    CHECK(wrap("語語語語語", 5, 3) == std::vector<std::string> { "語語", "語", "語", "語" });
}

namespace
{

/// An orphan on a branch, its path holding a space and a `'`.
constexpr auto QuotedOrphan = "/home/u/worktrees/contour/it's a test"sv;
/// An orphan on a detached HEAD.
constexpr auto DetachedOrphan = "/home/u/worktrees/contour/pr-2-20260101-000000"sv;
/// An orphan whose path holds an escape sequence; it would show as another path.
constexpr auto ControlOrphan = "/home/u/worktrees/contour/pr-3\x1b[31m"sv;

[[nodiscard]] World orphansWorld()
{
    return World { .worktrees = {
                       WorktreeEntry { .path = std::string { QuotedOrphan },
                                       .branch = "agent/pr-1-20260101-000000" },
                       WorktreeEntry { .path = std::string { DetachedOrphan }, .branch = std::nullopt },
                       WorktreeEntry { .path = std::string { ControlOrphan },
                                       .branch = "agent/pr-3-20260101-000000" },
                   } };
}

[[nodiscard]] Row const* rowOfOrphan(std::vector<Row> const& rows, std::string_view path)
{
    auto const found = std::ranges::find_if(
        rows, [path](Row const& row) { return row.kind == RowKind::Orphan && row.title == path; });
    return found != rows.end() ? &*found : nullptr;
}

[[nodiscard]] DetailLine const* removeCommandOf(Row const& row)
{
    auto const found = std::ranges::find_if(
        row.details, [](DetailLine const& line) { return line.value.starts_with("git -C "); });
    return found != row.details.end() ? &*found : nullptr;
}

/// The words a POSIX shell reads from @p line: blanks separate them, single quotes keep everything up to the
/// next one, a backslash outside them keeps the character after it.
[[nodiscard]] std::vector<std::string> shellWords(std::string_view line)
{
    auto words = std::vector<std::string> {};
    auto word = std::optional<std::string> {};
    auto quoted = false;
    auto escaped = false;
    for (auto const c: line)
    {
        if (escaped)
        {
            word = word.value_or("") + c;
            escaped = false;
        }
        else if (quoted)
        {
            if (c == '\'')
                quoted = false;
            else
                *word += c;
        }
        else if (c == '\'')
        {
            quoted = true;
            word = word.value_or("");
        }
        else if (c == '\\')
            escaped = true;
        else if (c == ' ')
        {
            if (word)
                words.push_back(*std::exchange(word, std::nullopt));
        }
        else
            word = word.value_or("") + c;
    }
    if (word)
        words.push_back(*word);
    return words;
}

} // namespace

TEST_CASE("orphans have their own section", "[orchestrator][view]")
{
    auto const engine = bootEngine(orphansWorld());
    REQUIRE(engine.orphans("contour").size() == 3);
    auto const rows = buildRows(engine, StartMonoMs);
    CHECK(textsOf(rows, RowKind::Heading).back() == "LEFTOVER WORKTREES");
    CHECK(rows.back().kind == RowKind::NewTask);
    CHECK(std::ranges::count(rows, RowKind::Orphan, &Row::kind) == 3);

    auto const* const quoted = rowOfOrphan(rows, QuotedOrphan);
    REQUIRE(quoted != nullptr);
    CHECK(quoted->text == QuotedOrphan);
    CHECK(quoted->status == "agent/pr-1-20260101-000000");
    CHECK(quoted->isSelectable());
    // Read-only: no key acts on it; its Focus shows its details.
    CHECK(quoted->offers == std::vector { KeyCommand::DetailsTab });
    CHECK(quoted->run.empty());

    // The command that removes it by hand pastes as one word per argument, whatever the path holds.
    // One line that works from any directory: git is told the checkout.
    auto const* const command = removeCommandOf(*quoted);
    REQUIRE(command != nullptr);
    CHECK(command->value
          == R"(git -C /home/u/projects/contour worktree remove '/home/u/worktrees/contour/it'\''s a test')");
    auto const argv = std::vector<std::string> { "git",      "-C",     std::string { ContourPath },
                                                 "worktree", "remove", std::string { QuotedOrphan } };
    CHECK(command->value == shellQuoted(argv));
    CHECK(shellWords(command->value) == argv);
    // An untrusted path is cut to its cells, never wrapped.
    CHECK(command->fit == DetailFit::Cut);
    CHECK(command->label == "to remove");

    auto const* const detached = rowOfOrphan(rows, DetachedOrphan);
    REQUIRE(detached != nullptr);
    CHECK(detached->status == "detached");
    REQUIRE(removeCommandOf(*detached) != nullptr);
    CHECK(removeCommandOf(*detached)->value
          == std::format("git -C {} worktree remove {}", ContourPath, DetachedOrphan));
    // Its commits are on no branch: removing it loses them, and the details say so.
    CHECK(std::ranges::any_of(detached->details, [](DetailLine const& line) {
        return line.role == Role::Warning && line.value.contains("detached");
    }));
}

TEST_CASE("an orphan whose path cannot be shown as it is offers no command", "[orchestrator][view]")
{
    // Sanitised, the path reads as another one; a command built from it could name another worktree.
    auto const engine = bootEngine(orphansWorld());
    auto const rows = buildRows(engine, StartMonoMs);
    auto const found = std::ranges::find_if(rows, [](Row const& row) {
        return row.kind == RowKind::Orphan && row.status == "agent/pr-3-20260101-000000";
    });
    REQUIRE(found != rows.end());
    CHECK(found->text == "/home/u/worktrees/contour/pr-3[31m");
    CHECK(removeCommandOf(*found) == nullptr);
    CHECK(std::ranges::any_of(found->details, [](DetailLine const& line) {
        return line.role == Role::Warning && line.value.contains("git worktree list");
    }));
    for (auto const& line: found->details)
        CHECK(!line.value.contains('\x1b'));
}

TEST_CASE("no orphan, no section", "[orchestrator][view]")
{
    auto const engine = bootEngine({});
    REQUIRE(engine.orphans("contour").empty());
    auto const rows = buildRows(engine, StartMonoMs);
    CHECK(!std::ranges::contains(textsOf(rows, RowKind::Heading), "LEFTOVER WORKTREES"));
    CHECK(std::ranges::none_of(rows, [](Row const& row) { return row.kind == RowKind::Orphan; }));
}

TEST_CASE("an orphan whose path holds a character that draws as nothing offers no command",
          "[orchestrator][view]")
{
    // `pr-4` and `pr-4` with a zero-width space look the same; the command would name the other one.
    auto const path = std::string { "/home/u/worktrees/contour/pr-4\u200B" };
    auto const engine = bootEngine(
        World { .worktrees = { WorktreeEntry { .path = path, .branch = "agent/pr-4-20260101-000000" } } });
    auto const rows = buildRows(engine, StartMonoMs);
    auto const* const row = rowOfOrphan(rows, path);
    REQUIRE(row != nullptr);
    CHECK(removeCommandOf(*row) == nullptr);
}

TEST_CASE("a long orphan path keeps its whole command", "[orchestrator][view]")
{
    // The panel cuts it when drawn; the row itself holds every byte, so a wider panel shows more.
    auto const path = std::format("/home/u/worktrees/contour/{}", std::string(600, 'x'));
    auto const engine =
        bootEngine(World { .worktrees = { WorktreeEntry { .path = path, .branch = std::nullopt } } });
    auto const rows = buildRows(engine, StartMonoMs);
    auto const found = std::ranges::find_if(rows, [](Row const& row) { return row.kind == RowKind::Orphan; });
    REQUIRE(found != rows.end());
    auto const* const command = removeCommandOf(*found);
    REQUIRE(command != nullptr);
    CHECK(command->value == std::format("git -C {} worktree remove {}", ContourPath, path));
}

TEST_CASE("a failed run says why in plain words and keeps the code in its state", "[orchestrator][view]")
{
    auto failed = storedRun(Finished { .ended = Ended::PreparationFailed,
                                       .fate = Fate::Removed,
                                       .entries = {},
                                       .seen = Seen::New,
                                       .endedWallMs = DispatchWallMs },
                            KeptId,
                            IssueRef { 2356 },
                            "issue");
    failed.failure = RunFailure { .step = PrepareStep::OpenPane, .why = "not-supported-yet" };
    auto const engine = bootEngine({ .stored = { failed } });
    auto const rows = buildRows(engine, StartMonoMs);
    auto const* const row = rowOfRun(rows, KeptId);
    REQUIRE(row != nullptr);
    CHECK(row->status == "failed");
    auto const detail = [&](std::string_view label) {
        auto const found = std::ranges::find(row->details, label, &DetailLine::label);
        return found != row->details.end() ? found->value : std::string {};
    };
    CHECK(detail("failure") == "agents can't start here yet");
    auto const failure = std::ranges::find(row->details, "failure"sv, &DetailLine::label);
    REQUIRE(std::next(failure) != row->details.end());
    CHECK(std::next(failure)->value.contains("Flatpak or Windows build"));
    CHECK(detail("state").ends_with(": pane: not-supported-yet"));
}

TEST_CASE("a 2 KiB unbroken word wraps to the same lines as cluster by cluster", "[orchestrator][view]")
{
    auto word = std::string {};
    while (word.size() < 2048)
        word += R"("\x)";
    word.resize(2048);
    // Printable ASCII is broken in slices: the first line 40 cells, every further one 36, the last the rest.
    auto expected = std::vector<std::string> { word.substr(0, 40) };
    for (auto const index: std::views::iota(std::size_t { 0 }, (word.size() - 40 + 35) / 36))
        expected.push_back(word.substr(40 + (index * 36), 36));
    CHECK(wrap(word, 40, 36) == expected);
    // After a short word, the long one starts on a line of its own, as before.
    auto after = std::vector<std::string> { "ab" };
    for (auto const index: std::views::iota(std::size_t { 0 }, (word.size() + 35) / 36))
        after.push_back(word.substr(index * 36, 36));
    CHECK(wrap("ab " + word, 40, 36) == after);
    // A word that is not ASCII alone is still broken between clusters.
    CHECK(wrap("日本語日本語", 4, 4) == std::vector<std::string> { "日本", "語日", "本語" });
}

TEST_CASE("a run a rule began is marked, and its details say which rule", "[orchestrator][view][automation]")
{
    auto byRule = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    byRule.origin = RunOrigin::Rule;
    byRule.originRule = "pipeline_failed/fix_ci";
    auto byGroup = storedRun(Running {}, DoneId, ChangeRequestRef { 2379 });
    byGroup.origin = RunOrigin::Bulk;
    auto const engine = bootEngine({ .stored = { byRule, byGroup } });
    auto const rows = buildRows(engine, StartMonoMs);

    auto const* const rule = rowOfRun(rows, RunningId);
    REQUIRE(rule != nullptr);
    CHECK(rule->text.starts_with("⚙ "));
    auto const startedAt = detailIndexOf(rule->details, "started by");
    REQUIRE(startedAt < rule->details.size());
    CHECK(rule->details[startedAt].value == "rule pipeline_failed → fix CI");

    auto const* const group = rowOfRun(rows, DoneId);
    REQUIRE(group != nullptr);
    CHECK(group->text.starts_with("⚙ "));
    auto const groupAt = detailIndexOf(group->details, "started by");
    REQUIRE(groupAt < group->details.size());
    CHECK(group->details[groupAt].value == "a group start");
}

TEST_CASE("a run the user began has no mark and no started-by line", "[orchestrator][view][automation]")
{
    auto const engine =
        bootEngine({ .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }) } });
    auto const rows = buildRows(engine, StartMonoMs);
    auto const* const row = rowOfRun(rows, RunningId);
    REQUIRE(row != nullptr);
    CHECK_FALSE(row->text.starts_with("⚙"));
    CHECK(detailIndexOf(row->details, "started by") == row->details.size());
}
