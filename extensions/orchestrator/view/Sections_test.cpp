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
    // Each section's items come under one group per repository, its header naming it and their count.
    CHECK(rows[1].kind == RowKind::Group);
    CHECK(rows[1].text == "contour · 5");
    CHECK(rows[1].group == "prs/contour");
    CHECK(rows[2].text.starts_with("#2375 "));
    CHECK(rows[2].group == "prs/contour");
}

TEST_CASE("a folded group lists its header, and its items only under a filter", "[orchestrator][view]")
{
    auto const header = [](Fold fold) {
        return Row {
            .kind = RowKind::Group, .text = "pace · 2", .repo = "pace", .group = "issues/pace", .fold = fold
        };
    };
    auto const item = [](std::string text, Fold fold) {
        return Row { .kind = RowKind::Issue,
                     .text = std::move(text),
                     .repo = "pace",
                     .group = "issues/pace",
                     .fold = fold };
    };
    auto const open = std::vector { header(Fold::Open),
                                    item("#1 crash on start", Fold::Open),
                                    item("#2 docs", Fold::Open) };
    auto const folded = std::vector { header(Fold::Folded),
                                      item("#1 crash on start", Fold::Folded),
                                      item("#2 docs", Fold::Folded) };

    // Open, the header is no stop for the selection; folded, it is the only one.
    CHECK(visibleRows(open, "", Furniture::Shown, ChartRows::Shown) == std::vector<std::size_t> { 0, 1, 2 });
    CHECK(selectableCount(open, visibleRows(open, "", Furniture::Shown, ChartRows::Shown)) == 2);
    CHECK(visibleRows(folded, "", Furniture::Shown, ChartRows::Shown) == std::vector<std::size_t> { 0 });
    CHECK(selectableCount(folded, visibleRows(folded, "", Furniture::Shown, ChartRows::Shown)) == 1);

    // A filter looks inside a folded group; a group none of whose items holds it goes, header and all.
    CHECK(visibleRows(folded, "crash", Furniture::Shown, ChartRows::Shown)
          == std::vector<std::size_t> { 0, 1 });
    CHECK(visibleRows(folded, "nothing", Furniture::Shown, ChartRows::Shown).empty());
    // The repository's name finds its items, now that they no longer carry it.
    CHECK(visibleRows(open, "pace", Furniture::Shown, ChartRows::Shown)
          == std::vector<std::size_t> { 0, 1, 2 });
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
    CHECK(running->offers == std::vector { KeyCommand::Board, KeyCommand::Transcript, KeyCommand::EndRun });

    auto const* const done = rowOfRun(rows, DoneId);
    REQUIRE(done != nullptr);
    CHECK(done->status == "done · e ends it");
    CHECK(done->offers == std::vector { KeyCommand::Board, KeyCommand::Transcript, KeyCommand::EndRun });

    auto const* const finished = rowOfRun(rows, KeptId);
    REQUIRE(finished != nullptr);
    CHECK(finished->kind == RowKind::Finished);
    CHECK(finished->mark == "●");
    CHECK(finished->text == "issue-2356 · work on issue · ended by you");
    CHECK(finished->status == "kept: 2 entries");
    CHECK(finished->offers
          == std::vector { KeyCommand::Board,
                           KeyCommand::Transcript,
                           KeyCommand::Dispatch,
                           KeyCommand::RetryRemoval,
                           KeyCommand::Dismiss });
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
    CHECK(active->offers == std::vector { KeyCommand::Board, KeyCommand::Transcript, KeyCommand::EndRun });
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
        // A removed run's board went with its worktree.
        auto const expected =
            id == KeptId ? std::vector { KeyCommand::Board,
                                         KeyCommand::Transcript,
                                         KeyCommand::Dispatch,
                                         KeyCommand::Dismiss }
                         : std::vector { KeyCommand::Transcript, KeyCommand::Dispatch, KeyCommand::Dismiss };
        CHECK(finished->offers == expected);
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
    CHECK(row->offers == std::vector { KeyCommand::Board, KeyCommand::Transcript, KeyCommand::EndRun });
    CHECK(std::ranges::none_of(row->details, [](DetailLine const& line) {
        return line.value.contains("outside the configuration");
    }));
    CHECK(nextRowChange(engine, StartMonoMs + 29'000) == StartMonoMs + 30'000);
}

TEST_CASE("with more than one repository each has its own group, which names it", "[orchestrator][view]")
{
    auto const engine = bootEngine({
        .repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github", "issues": [] },
                      { "name": "other", "path": "~/projects/other", "forge": "github", "issues": [] } ])",
        .repoPaths = { std::string { ContourPath }, std::string { OtherPath } },
    });
    auto const rows = buildRows(engine, StartMonoMs);
    auto groups = std::vector<std::string> {};
    for (auto const& row: rows)
        if (row.kind == RowKind::PullRequest)
        {
            groups.push_back(row.group);
            // The group's header names the repository; the row no longer does.
            CHECK(row.prefix.empty());
        }
    CHECK(std::ranges::count(groups, "prs/contour") == 5);
    CHECK(std::ranges::count(groups, "prs/other") == 5);
    CHECK(std::ranges::is_sorted(groups)); // in the configured order: contour first
    CHECK(textsOf(rows, RowKind::Group) == std::vector<std::string> { "contour · 5", "other · 5" });
}

TEST_CASE("a filter keeps the items that hold it; a collapsed layout drops the furniture",
          "[orchestrator][view]")
{
    auto const engine = bootEngine({});
    auto const rows = buildRows(engine, StartMonoMs);
    auto const at = [&](std::size_t index) -> Row const& {
        return rows[index];
    };
    auto const filtered = visibleRows(rows, "STDIO", Furniture::Shown, ChartRows::Shown);
    CHECK(selectableCount(rows, filtered) == 1);
    CHECK(
        std::ranges::count_if(filtered, [&](std::size_t index) { return at(index).kind == RowKind::Heading; })
        == 4);
    auto const collapsed = visibleRows(rows, "", Furniture::Collapsed, ChartRows::Shown);
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
    CHECK(rows.back().kind == RowKind::Orphan);
    CHECK(std::ranges::count(rows, RowKind::Orphan, &Row::kind) == 3);

    auto const* const quoted = rowOfOrphan(rows, QuotedOrphan);
    REQUIRE(quoted != nullptr);
    CHECK(quoted->text == QuotedOrphan);
    CHECK(quoted->status == "agent/pr-1-20260101-000000");
    CHECK(quoted->isSelectable());
    // Read-only: no key acts on it; Enter and Tab show its details.
    CHECK(quoted->offers.empty());
    CHECK(quoted->onOpen == OpenEffect::None);
    CHECK(quoted->run.empty());
    CHECK(!quoted->item);

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

TEST_CASE("a chart that is not drawn takes its heading with it", "[orchestrator][view]")
{
    auto const rows = std::vector {
        Row { .kind = RowKind::Heading, .text = "MY PULL REQUESTS" },
        Row { .kind = RowKind::Placeholder, .text = "no open pull requests" },
        Row { .kind = RowKind::Spacer },
        Row { .kind = RowKind::Heading, .text = "TIMELINE" },
        Row { .kind = RowKind::Chart, .prefix = "agents active", .text = "··█" },
        Row { .kind = RowKind::Spacer },
        Row { .kind = RowKind::Heading, .text = "AGENTS" },
    };
    CHECK(visibleRows(rows, "", Furniture::Shown, ChartRows::Shown).size() == rows.size());
    CHECK(visibleRows(rows, "", Furniture::Shown, ChartRows::Hidden)
          == std::vector<std::size_t> { 0, 1, 5, 6 });
}
