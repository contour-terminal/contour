// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("the sections come in order, each item under its heading", "[orchestrator][view]")
{
    auto const engine = bootEngine({});
    auto const rows = buildRows(engine, StartMonoMs);
    CHECK(textsOf(rows, RowKind::Heading)
          == std::vector<std::string> {
              "MY PULL REQUESTS", "ISSUES WITHOUT A PR", "AGENTS · 0 OF 4 RUNNING", "FINISHED · 0 NEW" });
    CHECK(std::ranges::count(rows, RowKind::PullRequest, &Row::kind) == 5);
    CHECK(std::ranges::count(rows, RowKind::Issue, &Row::kind)
          == static_cast<std::ptrdiff_t>(engine.unclaimedIssues("contour").size()));
    CHECK(textsOf(rows, RowKind::Placeholder)
          == std::vector<std::string> { "no agents", "nothing finished" });
    CHECK(rows.front().kind == RowKind::Heading);
    CHECK(rows[1].text.starts_with("#2375 "));
}

TEST_CASE("an unconfigured engine says what to configure", "[orchestrator][view]")
{
    auto const engine = bootEngine({ .configJson = "{}" });
    auto const rows = buildRows(engine, StartMonoMs);
    REQUIRE(!rows.empty());
    CHECK(std::ranges::all_of(rows, [](Row const& row) { return row.kind == RowKind::Notice; }));
    CHECK(std::ranges::any_of(rows, [](Row const& row) { return row.text.contains("worktree_root"); }));
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

    auto const* const running = rowOfRun(rows, RunningId);
    REQUIRE(running != nullptr);
    CHECK(running->text == "pr-2375 · fix CI · sonnet");
    CHECK(running->status == "working 6m");
    CHECK(running->offers == std::vector { KeyCommand::EndRun });

    auto const* const done = rowOfRun(rows, DoneId);
    REQUIRE(done != nullptr);
    CHECK(done->status == "done · e ends it");
    CHECK(done->offers == std::vector { KeyCommand::EndRun });

    auto const* const finished = rowOfRun(rows, KeptId);
    REQUIRE(finished != nullptr);
    CHECK(finished->kind == RowKind::Finished);
    CHECK(finished->mark == "●");
    CHECK(finished->text == "issue-2356 · work on issue · ended by you");
    CHECK(finished->status == "kept: 2 entries");
    CHECK(finished->offers == std::vector { KeyCommand::RetryRemoval, KeyCommand::Dismiss });
    CHECK(finished->onOpen == OpenEffect::MarkSeen);
    CHECK(std::ranges::contains(textsOf(rows, RowKind::Heading), "AGENTS · 2 OF 4 RUNNING"));
    CHECK(std::ranges::contains(textsOf(rows, RowKind::Heading), "FINISHED · 1 NEW"));
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
    CHECK(active->offers == std::vector { KeyCommand::EndRun });
    CHECK(active->details.front() == outside);

    // Its outcome wins in the row; the details still say where it belongs.
    struct Expected
    {
        std::string_view id;
        std::string_view status;
    };
    for (auto const& [id, status]: { Expected { .id = KeptId, .status = "kept: 1 entry" },
                                     Expected { .id = RemovedId, .status = "removed" } })
    {
        CAPTURE(id);
        auto const* const finished = rowOfRun(rows, id);
        REQUIRE(finished != nullptr);
        CHECK(finished->kind == RowKind::Finished);
        CHECK(finished->status == status);
        CHECK(finished->offers == std::vector { KeyCommand::Dismiss });
        CHECK(finished->details.front() == outside);
        CHECK(std::ranges::none_of(finished->details,
                                   [](DetailLine const& line) { return line.value.contains("x tries"); }));
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
    CHECK(row->status == runStateText(engine.runs().front()));
    CHECK(row->status != "outside the configuration");
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
    CHECK(std::ranges::contains(textsOf(rows, RowKind::Heading), "AGENTS · 1 RUNNING"));
    auto const* const row = rowOfRun(rows, RunningId);
    REQUIRE(row != nullptr);
    CHECK(row->status == "waiting? see its tab");
    CHECK(row->offers == std::vector { KeyCommand::EndRun });
    CHECK(std::ranges::none_of(row->details, [](DetailLine const& line) {
        return line.value.contains("outside the configuration");
    }));
    CHECK(nextRowChange(engine, StartMonoMs + 29'000) == StartMonoMs + 30'000);
}

TEST_CASE("with more than one repository a row names its own", "[orchestrator][view]")
{
    auto const engine = bootEngine({
        .repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github", "issues": [] },
                      { "name": "other", "path": "~/projects/other", "forge": "github", "issues": [] } ])",
        .repoPaths = { std::string { ContourPath }, std::string { OtherPath } },
    });
    auto const rows = buildRows(engine, StartMonoMs);
    auto prefixes = std::vector<std::string> {};
    for (auto const& row: rows)
        if (row.kind == RowKind::PullRequest)
            prefixes.push_back(row.prefix);
    CHECK(std::ranges::count(prefixes, "contour") == 5);
    CHECK(std::ranges::count(prefixes, "other") == 5);
    CHECK(std::ranges::is_sorted(prefixes)); // in the configured order: contour first
}

TEST_CASE("a filter keeps the items that hold it; a collapsed layout drops the furniture",
          "[orchestrator][view]")
{
    auto const engine = bootEngine({});
    auto const rows = buildRows(engine, StartMonoMs);
    auto const at = [&](std::size_t index) -> Row const& {
        return rows[index];
    };
    auto const filtered = visibleRows(rows, "STDIO", Furniture::Shown);
    CHECK(selectableCount(rows, filtered) == 1);
    CHECK(
        std::ranges::count_if(filtered, [&](std::size_t index) { return at(index).kind == RowKind::Heading; })
        == 4);
    auto const collapsed = visibleRows(rows, "", Furniture::Collapsed);
    CHECK(std::ranges::none_of(collapsed, [&](std::size_t index) { return at(index).isFurniture(); }));
    CHECK(selectableCount(rows, collapsed)
          == static_cast<std::size_t>(std::ranges::count_if(rows, &Row::isSelectable)));
    CHECK(positionOfSelectable(rows, filtered, 0) < filtered.size());
    CHECK(positionOfSelectable(rows, filtered, 1) == filtered.size());
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
