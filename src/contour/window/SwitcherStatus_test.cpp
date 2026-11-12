// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarTree.hpp>
#include <contour/window/StatusKinds.hpp>
#include <contour/window/StatusUrgency.hpp>
#include <contour/window/SwitcherStatus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtcontrol/Api.hpp>

using namespace contour::window;
using namespace std::chrono_literals;

namespace
{

/// 2026-11-12T10:00:00Z.
constexpr auto Noon = std::chrono::sys_days { std::chrono::year { 2026 } / std::chrono::November / 12 } + 10h;

/// A pane with @p records and @p lastFinished.
[[nodiscard]] vtcontrol::PaneInfo paneWith(std::vector<vtcontrol::ProgramStatusInfo> records,
                                           std::optional<vtcontrol::BlockInfo> lastFinished = std::nullopt)
{
    auto pane = vtcontrol::PaneInfo {};
    pane.id = "pane:p";
    pane.programStatus = std::move(records);
    pane.lastFinished = std::move(lastFinished);
    return pane;
}

/// A workspace whose one tab holds @p panes.
[[nodiscard]] vtcontrol::WorkspaceInfo workspaceWith(std::vector<vtcontrol::PaneInfo> panes)
{
    auto tab = vtcontrol::TabInfo {};
    tab.id = "tab:t";
    tab.panes = std::move(panes);
    auto workspace = vtcontrol::WorkspaceInfo {};
    workspace.id = "workspace:w";
    workspace.tabs.push_back(std::move(tab));
    return workspace;
}

/// A program-status record in @p state.
[[nodiscard]] vtcontrol::ProgramStatusInfo record(vtcontrol::ProgramState state,
                                                  std::string title = {},
                                                  std::string message = {},
                                                  std::string since = {},
                                                  vtcontrol::BlockedKind kind = vtcontrol::BlockedKind::None)
{
    auto info = vtcontrol::ProgramStatusInfo {};
    info.state = state;
    info.kind = kind;
    info.title = std::move(title);
    info.message = std::move(message);
    info.since = std::move(since);
    return info;
}

} // namespace

TEST_CASE("RemoteStatusRules list every signal once, in enumerator order", "[switcher]")
{
    STATIC_REQUIRE(RemoteStatusRules.size() == static_cast<std::size_t>(RemoteSignal::Blocked) + 1);
    CHECK(remoteStatusRule(RemoteSignal::None).urgency == Urgency::Idle);
    CHECK(remoteStatusRule(RemoteSignal::Working).glyph == TabBadgeKind::Working);
    CHECK(remoteStatusRule(RemoteSignal::ProgramDone).urgency == Urgency::DoneUnseen);
    CHECK(remoteStatusRule(RemoteSignal::CommandFailed).glyph == TabBadgeKind::Error);
    CHECK(remoteStatusRule(RemoteSignal::ProgramError).urgency == Urgency::Failed);
    CHECK(remoteStatusRule(RemoteSignal::Blocked).glyph == TabBadgeKind::Blocked);
}

TEST_CASE("switcherStatusOf takes a workspace row's urgency, glyph, git, reason and time", "[switcher]")
{
    auto row = SidebarRow {};
    row.urgency = Urgency::Failed;
    row.glyph = TabBadgeKind::Error;
    row.gitText = "main";
    row.reasonLabel = "exit 1";
    row.reasonSeparator = " · ";
    row.reasonCommand = "make deploy";
    row.time.text = "2m ago";
    CHECK(switcherStatusOf(row)
          == SwitcherStatus { .urgency = Urgency::Failed,
                              .glyph = TabBadgeKind::Error,
                              .gitText = "main",
                              .reason = { .label = "exit 1", .separator = " · ", .command = "make deploy" },
                              .time = { .kind = TimeKind::None, .text = "2m ago", .percent = std::nullopt },
                              .since = std::nullopt });
}

TEST_CASE("switcherStatusOf ranks another daemon's panes by RemoteStatusRules", "[switcher]")
{
    auto const workspace = workspaceWith({ paneWith({ record(vtcontrol::ProgramState::Working, "Building") }),
                                           paneWith({ record(vtcontrol::ProgramState::Blocked,
                                                             "Waiting",
                                                             "Bash: make deploy",
                                                             "2026-11-12T10:00:00.250Z",
                                                             vtcontrol::BlockedKind::Permission) }) });
    auto const status = switcherStatusOf(workspace);
    CHECK(status.urgency == Urgency::NeedsYou);
    CHECK(status.glyph == TabBadgeKind::Blocked);
    // The sidebar's reason: the kind and the tool as words, the command it asks to run as the command part.
    CHECK(status.reason
          == ReasonParts { .label = "Permission · Bash: ", .separator = {}, .command = "make deploy" });
    CHECK(status.gitText.empty());
    CHECK(status.time.kind == TimeKind::Live);
    CHECK(status.since == std::optional { Noon + 250ms });
    CHECK(switcherTimeText(status, Noon + 250ms + 252s) == "4:12");

    SECTION("a record's message wins over its title, as in the sidebar")
    {
        auto const done = switcherStatusOf(workspaceWith(
            { paneWith({ record(vtcontrol::ProgramState::Done, "Tests", "All tests pass") }) }));
        CHECK(done.urgency == Urgency::DoneUnseen);
        CHECK(done.glyph == TabBadgeKind::Done);
        CHECK(done.reason.label == "All tests pass");
        CHECK(done.time.kind == TimeKind::Relative);

        auto const titled =
            switcherStatusOf(workspaceWith({ paneWith({ record(vtcontrol::ProgramState::Done, "Tests") }) }));
        CHECK(titled.reason.label == "Tests");
    }

    SECTION("a working program's progress is its time")
    {
        auto working = record(vtcontrol::ProgramState::Working, "Building", {}, "2026-11-12T10:00:00Z");
        working.progress = 42;
        auto const building = switcherStatusOf(workspaceWith({ paneWith({ working }) }));
        CHECK(building.time.kind == TimeKind::Progress);
        CHECK(building.time.percent == std::optional<uint8_t> { 42 });
        CHECK(switcherTimeText(building, Noon + 1h) == "42%");
    }

    SECTION("an idle workspace asks for nothing")
    {
        auto const idle =
            switcherStatusOf(workspaceWith({ paneWith({ record(vtcontrol::ProgramState::Idle) }) }));
        CHECK(idle == SwitcherStatus {});
    }
}

TEST_CASE("a failed last command marks another daemon's workspace failed", "[switcher]")
{
    auto workspace = workspaceWith({ paneWith({},
                                              vtcontrol::BlockInfo { .id = "block:p/1",
                                                                     .pane = "pane:p",
                                                                     .state = vtcontrol::BlockState::Finished,
                                                                     .command = "make deploy",
                                                                     .exitCode = 1,
                                                                     .startedAt = "2026-11-12T10:00:00.000Z",
                                                                     .durationMs = 18'000 }) });
    workspace.git = vtcontrol::WorkspaceGitInfo { .branch = "main", .ahead = 2 };
    auto const status = switcherStatusOf(workspace);
    CHECK(status
          == SwitcherStatus {
              .urgency = Urgency::Failed,
              .glyph = TabBadgeKind::Error,
              .gitText = "main ↑2",
              .reason = { .label = "exit 1 · 18s", .separator = " · ", .command = "make deploy" },
              .time = {},
              .since = std::nullopt });
    // As the sidebar's finished reason: no age beside it.
    CHECK(switcherTimeText(status, Noon + 18s + 120s).empty());

    SECTION("a command that succeeded asks for nothing, and keeps the git text")
    {
        workspace.tabs[0].panes[0].lastFinished->exitCode = 0;
        CHECK(switcherStatusOf(workspace)
              == SwitcherStatus { .urgency = Urgency::Idle,
                                  .glyph = TabBadgeKind::None,
                                  .gitText = "main ↑2",
                                  .reason = {},
                                  .time = {},
                                  .since = std::nullopt });
    }

    SECTION("a blocked program outranks the failure")
    {
        workspace.tabs[0].panes[0].programStatus.push_back(
            record(vtcontrol::ProgramState::Blocked, "Question"));
        CHECK(switcherStatusOf(workspace).urgency == Urgency::NeedsYou);
    }
}

TEST_CASE("rollUpStatus keeps the most urgent part, the first on a tie", "[switcher]")
{
    auto const statusOf = [](Urgency urgency, TabBadgeKind glyph, std::string gitText) {
        auto status = SwitcherStatus {};
        status.urgency = urgency;
        status.glyph = glyph;
        status.gitText = std::move(gitText);
        return status;
    };
    auto const working = statusOf(Urgency::Working, TabBadgeKind::Working, {});
    auto const firstFailed = statusOf(Urgency::Failed, TabBadgeKind::Error, "first");
    auto const secondFailed = statusOf(Urgency::Failed, TabBadgeKind::Error, "second");
    auto const parts = std::array { working, firstFailed, secondFailed };
    CHECK(rollUpStatus(parts) == firstFailed);
    CHECK(rollUpStatus(std::span<SwitcherStatus const> {}) == SwitcherStatus {});
}

TEST_CASE("switcherTimeText prefers the preformatted text, else formats since as its kind says", "[switcher]")
{
    auto const now = Noon;
    auto preformatted = SwitcherStatus {};
    preformatted.time.text = "4:12";
    preformatted.since = now - 1h;
    CHECK(switcherTimeText(preformatted, now) == "4:12");
    auto remote = SwitcherStatus {};
    remote.time.kind = TimeKind::Relative;
    remote.since = now - 120s;
    CHECK(switcherTimeText(remote, now) == formatRelative(120s));
    remote.time.kind = TimeKind::Live;
    CHECK(switcherTimeText(remote, now) == formatLive(120s));
    CHECK(switcherTimeText(SwitcherStatus {}, now).empty());
}

TEST_CASE("parseRfc3339Utc reads the control API's timestamps", "[switcher]")
{
    struct Case
    {
        std::string_view text;
        std::optional<std::chrono::system_clock::time_point> expected;
    };
    auto const cases = std::array {
        Case { .text = "2026-11-12T10:00:00.000Z", .expected = Noon },
        Case { .text = "2026-11-12T10:00:00.250Z", .expected = Noon + 250ms },
        Case { .text = "2026-11-12T10:00:00Z", .expected = Noon },
        Case { .text = "2026-11-12T10:00:00.5Z", .expected = Noon + 500ms },
        Case { .text = "", .expected = std::nullopt },
        Case { .text = "2026-11-12", .expected = std::nullopt },
        Case { .text = "2026-13-12T10:00:00Z", .expected = std::nullopt },
        Case { .text = "2026-11-12T25:00:00Z", .expected = std::nullopt },
        Case { .text = "2026-11-12T10:00:00+01:00", .expected = std::nullopt },
        Case { .text = "2026-11-12T10:00:00.000Zjunk", .expected = std::nullopt },
    };
    for (auto const& c: cases)
    {
        INFO(c.text);
        CHECK(parseRfc3339Utc(c.text) == c.expected);
    }
}
