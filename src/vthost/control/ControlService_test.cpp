// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Terminal.hpp>

#include <vtpty/MockPty.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vtcontrol/Json.hpp>
#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/testing/FakeControlApi.hpp>
#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/control/ControlService.hpp>
#include <vthost/testing/ControlHarness.hpp>
#include <vthost/testing/TestHelpers.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

using vtcontrol::ControlError;
using vtcontrol::ControlErrorCode;
using vtcontrol::testing::FakeCallContext;
using vthost::control::ControlService;

namespace
{

using vthost::testing::ControlHarness;

/// Calls one method and returns what it answered with, which it must answer at once.
/// @tparam Reply The method's reply type.
template <typename Reply, typename Request>
[[nodiscard]] std::expected<Reply, ControlError> callSync(ControlService& service,
                                                          vtcontrol::CallContext& context,
                                                          Request const& request)
{
    auto result = std::optional<std::expected<Reply, ControlError>> {};
    service.handle(
        context, request, [&](std::expected<Reply, ControlError> reply) { result = std::move(reply); });
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// One full OSC 133 cycle: a prompt, `command` typed, its output, and its exit status.
[[nodiscard]] std::string commandCycle(std::string_view command, std::string_view output, int exitCode)
{
    return std::format("\033]133;A\007$ \033]133;B\007{}\r\n\033]133;C\007{}\r\n\033]133;D;{}\007",
                       command,
                       output,
                       exitCode);
}

} // namespace

// The daemon's workspace count is what a client's switcher lists: the workspaces that hold a tab. Both read
// the listing's tabs, so a workspace the daemon counts is one `workspace.list` shows with a tab.
TEST_CASE("the populated workspace count is the number of listed workspaces that hold a tab",
          "[vthost][control]")
{
    auto h = ControlHarness {};
    h.openTab();
    REQUIRE(h.host.model().createWorkspace() != nullptr); // empty, as a fresh daemon's is

    auto const list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    REQUIRE(list.has_value());
    REQUIRE(list->workspaces.size() == 2);
    auto const holdingATab = static_cast<std::size_t>(std::ranges::count_if(
        list->workspaces, [](auto const& workspace) { return !workspace.tabs.empty(); }));
    CHECK(holdingATab == 1);
    CHECK(h.host.populatedWorkspaceCount() == holdingATab);
}

TEST_CASE("workspace.list mirrors the model with registry ids", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const first = h.openTab();
    auto* tab = h.host.model().workspace(h.host.workspaceId())->activeTab();
    h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
    auto const second = tab->activePane()->session();

    auto const list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    REQUIRE(list.has_value());
    REQUIRE(list->workspaces.size() == 1);
    CHECK(list->workspaces[0].id
          == vtcontrol::toString(
              vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace,
                                     .uuid = *h.host.objects().uuidOf(h.host.workspaceId()) }));
    CHECK(list->workspaces[0].position == 1);
    REQUIRE(list->workspaces[0].tabs.size() == 1);
    auto const& tabInfo = list->workspaces[0].tabs[0];
    CHECK(tabInfo.isActive);
    CHECK(tabInfo.position == 1);
    REQUIRE(tabInfo.panes.size() == 2);
    CHECK(tabInfo.panes[0].id == h.paneId(first));
    CHECK_FALSE(tabInfo.panes[0].isActive);
    CHECK(tabInfo.panes[1].id == h.paneId(second));
    CHECK(tabInfo.panes[1].isActive);
}

TEST_CASE("daemon.info reports the configured version", "[vthost][control]")
{
    auto h = ControlHarness {};
    h.wall.advance(std::chrono::seconds { 42 });
    auto const info =
        callSync<vtcontrol::DaemonInfoReply>(h.service, h.context, vtcontrol::DaemonInfoRequest {});
    REQUIRE(info.has_value());
    CHECK(info->version == "9.8.7");
    CHECK(info->protocolVersion > 0);
    CHECK_FALSE(info->platform.empty());
    CHECK(info->uptimeSeconds == 42);
}

TEST_CASE("daemon.upgrade is refused to a remote caller, and without an upgrade to start",
          "[vthost][control][upgrade]")
{
    auto h = ControlHarness {};
    SECTION("a remote caller is refused localOnly")
    {
        h.context.from = vtcontrol::CallerLocality::Remote;
        auto const upgraded =
            callSync<vtcontrol::DaemonUpgradeReply>(h.service, h.context, vtcontrol::DaemonUpgradeRequest {});
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().message.starts_with("localOnly: "));
    }
    SECTION("a service that starts no upgrade refuses notSupported")
    {
        auto const upgraded =
            callSync<vtcontrol::DaemonUpgradeReply>(h.service, h.context, vtcontrol::DaemonUpgradeRequest {});
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().message.starts_with("notSupported: "));
    }
}

TEST_CASE("daemon.info reports the connection's grants", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const names = [&](vtcontrol::ControlGrants grants) {
        h.context.granted = grants;
        auto const info =
            callSync<vtcontrol::DaemonInfoReply>(h.service, h.context, vtcontrol::DaemonInfoRequest {});
        REQUIRE(info.has_value());
        return info->grants;
    };
    using vtcontrol::Permission;
    CHECK(names(vtcontrol::ControlGrants::of({ Permission::Read, Permission::Screen }))
          == std::vector<std::string> { "read", "screen" });
    CHECK(names(vtcontrol::ControlGrants::all())
          == std::vector<std::string> { "read", "screen", "input", "write" });
}

TEST_CASE("pane.capture reads the page and scrollback", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();

    SECTION("the page")
    {
        h.terminal(session).writeToScreen("hello\r\nworld");
        auto const capture =
            callSync<vtcontrol::CaptureReply>(h.service, h.context, vtcontrol::CaptureRequest {});
        REQUIRE(capture.has_value());
        CHECK(capture->text == "hello\nworld");
        CHECK(capture->pane == h.paneId(session));
        CHECK(capture->truncated == vtcontrol::Truncation::None);
        CHECK(capture->cursor.line == 1);
        CHECK(capture->cursor.column == 5);
        CHECK(capture->size.lines == unbox<uint32_t>(h.terminal(session).pageSize().lines));
    }

    SECTION("the lines that scrolled off")
    {
        auto const pageLines = unbox<int>(h.terminal(session).pageSize().lines);
        auto text = std::string {};
        for (auto const line: std::views::iota(1, pageLines + 6))
            text += std::format("line{}\r\n", line);
        h.terminal(session).writeToScreen(text);

        auto const page =
            callSync<vtcontrol::CaptureReply>(h.service, h.context, vtcontrol::CaptureRequest {});
        REQUIRE(page.has_value());
        CHECK_FALSE(page->text.starts_with("line1\n"));

        auto const two = callSync<vtcontrol::CaptureReply>(
            h.service, h.context, vtcontrol::CaptureRequest { .pane = {}, .scrollback = 2 });
        REQUIRE(two.has_value());
        CHECK(two->text.starts_with("line5\n"));

        auto const all = callSync<vtcontrol::CaptureReply>(
            h.service, h.context, vtcontrol::CaptureRequest { .pane = {}, .scrollback = -1 });
        REQUIRE(all.has_value());
        CHECK(all->text.starts_with("line1\nline2\n"));
        CHECK(all->text.ends_with(std::format("line{}", pageLines + 5)));
    }
}

TEST_CASE("pane.capture refuses a scrollback below -1", "[vthost][control]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto const capture = callSync<vtcontrol::CaptureReply>(
        h.service, h.context, vtcontrol::CaptureRequest { .pane = {}, .scrollback = -2 });
    REQUIRE_FALSE(capture.has_value());
    CHECK(capture.error().code == ControlErrorCode::BadRequest);
}

TEST_CASE("a stale caller pane is NotFound, never another pane", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const kept = h.openTab();
    auto* tab = h.host.model().workspace(h.host.workspaceId())->activeTab();
    h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
    auto const gone = tab->activePane()->session();
    auto const goneId = h.paneId(gone);
    h.context.callerPane = goneId;
    h.host.handleSessionExit(gone);
    REQUIRE(h.host.terminal(kept) != nullptr);

    auto const capture =
        callSync<vtcontrol::CaptureReply>(h.service, h.context, vtcontrol::CaptureRequest {});
    REQUIRE_FALSE(capture.has_value());
    CHECK(capture.error().code == ControlErrorCode::NotFound);
    CHECK(capture.error().message.contains(goneId));

    // An id copied from another daemon is just as unknown.
    auto const foreign = callSync<vtcontrol::CaptureReply>(
        h.service,
        h.context,
        vtcontrol::CaptureRequest { .pane = "pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ab", .scrollback = 0 });
    REQUIRE_FALSE(foreign.has_value());
    CHECK(foreign.error().code == ControlErrorCode::NotFound);
}

TEST_CASE("captured text is always valid JSON", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.terminal(session).writeToScreen(std::string_view { "a\xff\x1b[31mb\0c", 10 });

    auto const capture =
        callSync<vtcontrol::CaptureReply>(h.service, h.context, vtcontrol::CaptureRequest {});
    REQUIRE(capture.has_value());
    auto const json = vtcontrol::encodeJson(*capture);
    auto parsed = glz::generic {};
    CHECK_FALSE(glz::read_json(parsed, json));
    auto const decoded = vtcontrol::decodeJson<vtcontrol::CaptureReply>(json);
    REQUIRE(decoded.has_value());
    CHECK(decoded->text.contains("\xEF\xBF\xBD"));
    CHECK(decoded->text.starts_with("a"));
}

TEST_CASE("a capture larger than maxReplyBytes keeps its tail", "[vthost][control]")
{
    auto h = ControlHarness { 1024 };
    auto const session = h.openTab();
    auto text = std::string {};
    while (text.size() < 5000)
    {
        // GCC's link-time inlining reports a -Wstringop-overflow for the natural form of this (a string
        // temporary joined with `+`) that cannot happen, so it is built in pieces.
        text.append(70, 'x');
        text += "\r\n";
    }
    text += "LAST";
    h.terminal(session).writeToScreen(text);

    auto const capture = callSync<vtcontrol::CaptureReply>(
        h.service, h.context, vtcontrol::CaptureRequest { .pane = {}, .scrollback = -1 });
    REQUIRE(capture.has_value());
    CHECK(capture->text.size() <= 1024);
    CHECK(capture->truncated == vtcontrol::Truncation::Head);
    CHECK(capture->text.ends_with("LAST"));
}

TEST_CASE("block.list and block.read report a finished command", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.terminal(session).writeToScreen(commandCycle("make", "out", 3));

    auto const list =
        callSync<vtcontrol::BlockListReply>(h.service, h.context, vtcontrol::BlockListRequest {});
    REQUIRE(list.has_value());
    CHECK(list->pane == h.paneId(session));
    REQUIRE(list->blocks.size() == 1);
    auto const& block = list->blocks.front();
    CHECK(block.state == vtcontrol::BlockState::Finished);
    CHECK(block.exitCode == 3);
    CHECK(block.command == "make");
    CHECK(block.startedAt.has_value());
    auto const ref = vtcontrol::parseBlockRef(block.id);
    REQUIRE(ref.has_value());
    CHECK(ref->pane == *h.host.objects().uuidOf(session));

    auto const read = callSync<vtcontrol::BlockReadReply>(
        h.service,
        h.context,
        vtcontrol::BlockReadRequest { .block = block.id, .pane = {}, .part = vtcontrol::BlockPart::Output });
    REQUIRE(read.has_value());
    CHECK(read->output == "out");
    CHECK(read->prompt.empty());
    CHECK(read->block.id == block.id);

    // An empty block means the pane's last finished one.
    auto const last =
        callSync<vtcontrol::BlockReadReply>(h.service, h.context, vtcontrol::BlockReadRequest {});
    REQUIRE(last.has_value());
    CHECK(last->block.id == block.id);
    CHECK(last->output == "out");
    CHECK(last->prompt.starts_with("$ "));

    // A block of another epoch, or of no pane, is NotFound.
    auto const stale = callSync<vtcontrol::BlockReadReply>(
        h.service,
        h.context,
        vtcontrol::BlockReadRequest {
            .block = vtcontrol::toString(
                vtcontrol::BlockRef { .pane = ref->pane, .serial = ref->serial + (uint64_t { 7 } << 32) }),
            .pane = {},
            .part = vtcontrol::BlockPart::All });
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error().code == ControlErrorCode::NotFound);
}

TEST_CASE("object.setLabel makes a pane addressable by label", "[vthost][control]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto const logs = h.openTab();
    h.terminal(logs).writeToScreen("tail -f");

    auto const set = callSync<vtcontrol::EmptyReply>(
        h.service, h.context, vtcontrol::SetLabelRequest { .object = h.paneId(logs), .label = "logs" });
    REQUIRE(set.has_value());

    auto const capture = callSync<vtcontrol::CaptureReply>(
        h.service, h.context, vtcontrol::CaptureRequest { .pane = "LOGS", .scrollback = 0 });
    REQUIRE(capture.has_value());
    CHECK(capture->pane == h.paneId(logs));
    CHECK(capture->text == "tail -f");

    // The label is the spec of `<kind>:<spec>` too, and an empty label clears it.
    REQUIRE(callSync<vtcontrol::EmptyReply>(
                h.service, h.context, vtcontrol::SetLabelRequest { .object = "pane:logs", .label = "" })
                .has_value());
    auto const cleared = callSync<vtcontrol::CaptureReply>(
        h.service, h.context, vtcontrol::CaptureRequest { .pane = "logs", .scrollback = 0 });
    CHECK_FALSE(cleared.has_value());

    auto const bad = callSync<vtcontrol::EmptyReply>(
        h.service, h.context, vtcontrol::SetLabelRequest { .object = "frame:1", .label = "x" });
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ControlErrorCode::BadRequest);

    REQUIRE(callSync<vtcontrol::EmptyReply>(
                h.service, h.context, vtcontrol::SetLabelRequest { .object = "tab:2", .label = "build" })
                .has_value());
    auto const list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    REQUIRE(list.has_value());
    CHECK(list->workspaces[0].tabs[1].label == "build");
}

TEST_CASE("a control call from a pane moved into a new workspace resolves its tab", "[vthost][control][move]")
{
    auto h = ControlHarness {};
    auto const first = h.openTab();
    auto const dragged = h.openTab();
    auto const& workspace = h.host.createEmptyWorkspace();
    REQUIRE(h.host.model()
                .moveNode(vtworkspace::PaneSource { dragged },
                          vtworkspace::TabWellTarget { .workspace = workspace.id(), .index = 0 })
                .has_value());
    h.context.callerPane = h.paneId(dragged);

    REQUIRE(callSync<vtcontrol::EmptyReply>(
                h.service, h.context, vtcontrol::SetLabelRequest { .object = "tab:", .label = "moved" })
                .has_value());
    REQUIRE(callSync<vtcontrol::EmptyReply>(
                h.service, h.context, vtcontrol::SetLabelRequest { .object = "workspace:", .label = "away" })
                .has_value());
    CHECK(h.host.terminal(first) != nullptr);
}

TEST_CASE("tab.create returns ids that workspace.list then shows", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const first = h.openTab();

    auto const created = callSync<vtcontrol::CreateTabReply>(
        h.service,
        h.context,
        vtcontrol::CreateTabRequest { .label = "build", .focus = vtcontrol::FocusPolicy::Keep });
    REQUIRE(created.has_value());

    auto const list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    REQUIRE(list.has_value());
    REQUIRE(list->workspaces.size() == 1);
    CHECK(list->workspaces[0].id == created->workspace);
    REQUIRE(list->workspaces[0].tabs.size() == 2);
    auto const& tab = list->workspaces[0].tabs[1];
    CHECK(tab.id == created->tab);
    CHECK(tab.label == "build");
    REQUIRE(tab.panes.size() == 1);
    CHECK(tab.panes[0].id == created->pane);
    // Keep left the first tab the active one.
    CHECK(list->workspaces[0].tabs[0].isActive);
    CHECK(list->workspaces[0].tabs[0].panes[0].id == h.paneId(first));

    // Take, the default, activates it.
    auto const taken =
        callSync<vtcontrol::CreateTabReply>(h.service, h.context, vtcontrol::CreateTabRequest {});
    REQUIRE(taken.has_value());
    CHECK(h.host.model().workspace(h.host.workspaceId())->activeTabIndex() == 2);
}

namespace
{
struct StubViewer final: vthost::SessionStreamEvents
{
};
} // namespace

TEST_CASE("tab.create with placement new opens a second workspace", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();

    auto const created = callSync<vtcontrol::CreateTabReply>(
        h.service,
        h.context,
        vtcontrol::CreateTabRequest { .placement = vtcontrol::WorkspacePlacement::New,
                                      .command = { "htop" } });
    REQUIRE(created.has_value());
    REQUIRE(h.host.model().workspaceCount() == 2);
    auto const* workspace = h.host.model().workspaceAt(1);
    CHECK(created->workspace
          == vtcontrol::toString(vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace,
                                                        .uuid = *h.host.objects().uuidOf(workspace->id()) }));
    CHECK(h.seen.back().program == "htop");
    // No client claims it, so the primary viewer follows it (settled conflict #6).
    auto const viewer = StubViewer {};
    CHECK(h.host.workspaceClaim(workspace->id(), &viewer) == vthost::WorkspaceClaim::Unclaimed);

    auto const list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    REQUIRE(list.has_value());
    REQUIRE(list->workspaces.size() == 2);
    CHECK(list->workspaces[1].position == 2);
    CHECK(list->workspaces[1].tabs.at(0).panes.at(0).id == created->pane);

    // A tab in that workspace, named by its position.
    auto const second = callSync<vtcontrol::CreateTabReply>(
        h.service, h.context, vtcontrol::CreateTabRequest { .workspace = "2" });
    REQUIRE(second.has_value());
    CHECK(second->workspace == created->workspace);
    CHECK(h.host.model().workspaceAt(1)->tabCount() == 2);
}

TEST_CASE("pane.split below 30 gives the new pane 30 percent and focus", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const anchor = h.openTab();

    auto const split = callSync<vtcontrol::SplitReply>(
        h.service,
        h.context,
        vtcontrol::SplitRequest { .pane = h.paneId(anchor),
                                  .direction = vtcontrol::SplitDirection::Below,
                                  .ratioPercent = 30,
                                  .label = "tail" });
    REQUIRE(split.has_value());
    auto const* tab = h.host.model().workspace(h.host.workspaceId())->activeTab();
    REQUIRE(tab->paneCount() == 2);
    auto const* root = tab->rootPane();
    CHECK(root->splitState() == vtworkspace::SplitState::Horizontal);
    CHECK(root->ratio() == 0.7);
    CHECK(h.paneId(tab->activePane()->session()) == split->pane);
    CHECK(h.paneId(root->second()->session()) == split->pane);

    auto const byLabel = callSync<vtcontrol::CaptureReply>(
        h.service, h.context, vtcontrol::CaptureRequest { .pane = "tail", .scrollback = 0 });
    REQUIRE(byLabel.has_value());
    CHECK(byLabel->pane == split->pane);
}

TEST_CASE("pane.split with focus keep leaves the anchor active", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const anchor = h.openTab();
    auto const split = callSync<vtcontrol::SplitReply>(
        h.service,
        h.context,
        vtcontrol::SplitRequest { .pane = h.paneId(anchor), .focus = vtcontrol::FocusPolicy::Keep });
    REQUIRE(split.has_value());
    auto const* tab = h.host.model().workspace(h.host.workspaceId())->activeTab();
    CHECK(tab->paneCount() == 2);
    CHECK(tab->activePane()->session() == anchor);
    CHECK(tab->rootPane()->splitState() == vtworkspace::SplitState::Vertical);
    CHECK(tab->rootPane()->ratio() == 0.5);
}

TEST_CASE("pane.split refuses a ratio outside 5..95", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const anchor = h.openTab();
    for (auto const percent: { uint8_t { 99 }, uint8_t { 4 } })
    {
        auto const split = callSync<vtcontrol::SplitReply>(
            h.service,
            h.context,
            vtcontrol::SplitRequest { .pane = h.paneId(anchor), .ratioPercent = percent });
        REQUIRE_FALSE(split.has_value());
        CHECK(split.error().code == ControlErrorCode::BadRequest);
    }
    CHECK(h.host.sessionCount() == 1);
}

TEST_CASE("pane.split passes command and cwd to the factory", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    h.files.addDirectory("/tmp");
    auto const split = callSync<vtcontrol::SplitReply>(
        h.service, h.context, vtcontrol::SplitRequest { .cwd = "/tmp", .command = { "htop", "-d", "10" } });
    REQUIRE(split.has_value());
    REQUIRE(h.seen.size() == 2);
    CHECK(h.seen.back().program == "htop");
    CHECK(h.seen.back().arguments == std::vector<std::string> { "-d", "10" });
    CHECK(h.seen.back().workingDirectory == "/tmp");
    // The pane's ids still reach its shell.
    CHECK(h.seen.back().env.at("CONTOUR_PANE") == split->pane);
}

TEST_CASE("pane.split without cwd starts in the anchor's directory", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.files.addDirectory("/srv/app");
    vthost::testing::reportDirectory(h.terminal(session), "/srv/app");
    auto const split = callSync<vtcontrol::SplitReply>(h.service, h.context, vtcontrol::SplitRequest {});
    REQUIRE(split.has_value());
    REQUIRE(h.seen.size() == 2);
    CHECK(h.seen.back().workingDirectory == "/srv/app");
}

TEST_CASE("pane.split keeps the command and inherits the directory when only cwd is absent",
          "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.files.addDirectory("/srv/app");
    vthost::testing::reportDirectory(h.terminal(session), "/srv/app");
    auto const split = callSync<vtcontrol::SplitReply>(
        h.service, h.context, vtcontrol::SplitRequest { .command = { "htop" } });
    REQUIRE(split.has_value());
    CHECK(h.seen.back().program == "htop");
    CHECK(h.seen.back().workingDirectory == "/srv/app");
}

TEST_CASE("pane.split with cwd keeps the requested directory", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.files.addDirectory("/srv/app");
    h.files.addDirectory("/tmp");
    vthost::testing::reportDirectory(h.terminal(session), "/srv/app");
    auto const split =
        callSync<vtcontrol::SplitReply>(h.service, h.context, vtcontrol::SplitRequest { .cwd = "/tmp" });
    REQUIRE(split.has_value());
    CHECK(h.seen.back().workingDirectory == "/tmp");
}

TEST_CASE("tab.create from a pane starts in that pane's directory; from outside, in the default",
          "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.files.addDirectory("/srv/app");
    vthost::testing::reportDirectory(h.terminal(session), "/srv/app");

    auto const outside =
        callSync<vtcontrol::CreateTabReply>(h.service, h.context, vtcontrol::CreateTabRequest {});
    REQUIRE(outside.has_value());
    CHECK(h.seen.back().workingDirectory.empty());

    h.context.callerPane = h.paneId(session);
    auto const inside =
        callSync<vtcontrol::CreateTabReply>(h.service, h.context, vtcontrol::CreateTabRequest {});
    REQUIRE(inside.has_value());
    CHECK(h.seen.back().workingDirectory == "/srv/app");

    h.files.addDirectory("/tmp");
    auto const explicitCwd = callSync<vtcontrol::CreateTabReply>(
        h.service, h.context, vtcontrol::CreateTabRequest { .cwd = "/tmp" });
    REQUIRE(explicitCwd.has_value());
    CHECK(h.seen.back().workingDirectory == "/tmp");
}

TEST_CASE("pane.split with a cwd that does not exist still splits, in the profile's directory",
          "[vthost][control][write]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto const split =
        callSync<vtcontrol::SplitReply>(h.service, h.context, vtcontrol::SplitRequest { .cwd = "/gone" });
    REQUIRE(split.has_value());
    REQUIRE(h.seen.size() == 2);
    CHECK(h.seen.back().workingDirectory.empty());
}

TEST_CASE("pane.split below a pane whose directory is gone keeps the profile's, and still splits",
          "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.files.addDirectory("/srv/app");
    vthost::testing::reportDirectory(h.terminal(session), "/srv/app");
    REQUIRE(h.files.remove("/srv/app").value_or(false));
    auto const split = callSync<vtcontrol::SplitReply>(h.service, h.context, vtcontrol::SplitRequest {});
    REQUIRE(split.has_value());
    REQUIRE(h.seen.size() == 2);
    CHECK(h.seen.back().workingDirectory.empty());
}

TEST_CASE("tab.create into a new workspace does not inherit the caller's directory",
          "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.files.addDirectory("/srv/app");
    vthost::testing::reportDirectory(h.terminal(session), "/srv/app");
    h.context.callerPane = h.paneId(session);

    auto const created = callSync<vtcontrol::CreateTabReply>(
        h.service,
        h.context,
        vtcontrol::CreateTabRequest { .placement = vtcontrol::WorkspacePlacement::New });
    REQUIRE(created.has_value());
    CHECK(h.seen.back().workingDirectory.empty());
}

TEST_CASE("pane.send writes to the pane's PTY", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    auto const sent = callSync<vtcontrol::SendReply>(
        h.service, h.context, vtcontrol::SendRequest { .pane = h.paneId(session), .text = "ls\r" });
    REQUIRE(sent.has_value());
    CHECK_FALSE(sent->block.has_value());
    auto& tapped = dynamic_cast<vthost::TappingPty&>(h.terminal(session).device());
    auto& pty = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    CHECK(pty.stdinBuffer() == "ls\r");

    // An empty text is a successful no-op.
    REQUIRE(callSync<vtcontrol::SendReply>(
                h.service, h.context, vtcontrol::SendRequest { .pane = h.paneId(session) })
                .has_value());
    CHECK(pty.stdinBuffer() == "ls\r");
}

namespace
{
/// Remembers why each session a host ended ended.
struct EndCauses final: vthost::SessionStreamEvents
{
    std::vector<vthost::SessionEnd> causes {};

    void sessionEnded(vtworkspace::SessionId /*session*/, vthost::SessionEnd cause) override
    {
        causes.push_back(cause);
    }
};
} // namespace

TEST_CASE("pane.close and tab.close remove what they name", "[vthost][control][write]")
{
    auto h = ControlHarness {};
    auto ends = EndCauses {};
    h.host.subscribeStream(&ends);
    auto const kept = h.openTab();
    auto const split = callSync<vtcontrol::SplitReply>(h.service, h.context, vtcontrol::SplitRequest {});
    REQUIRE(split.has_value());
    REQUIRE(h.host.sessionCount() == 2);

    REQUIRE(callSync<vtcontrol::EmptyReply>(
                h.service, h.context, vtcontrol::ClosePaneRequest { .pane = split->pane })
                .has_value());
    CHECK(h.host.sessionCount() == 1);
    CHECK(h.host.terminal(kept) != nullptr);
    // What is closed is gone, not replaced.
    auto const again = callSync<vtcontrol::EmptyReply>(
        h.service, h.context, vtcontrol::ClosePaneRequest { .pane = split->pane });
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == ControlErrorCode::NotFound);

    auto const second =
        callSync<vtcontrol::CreateTabReply>(h.service, h.context, vtcontrol::CreateTabRequest {});
    REQUIRE(second.has_value());
    REQUIRE(callSync<vtcontrol::SplitReply>(
                h.service, h.context, vtcontrol::SplitRequest { .pane = second->pane })
                .has_value());
    REQUIRE(h.host.sessionCount() == 3);
    REQUIRE(callSync<vtcontrol::EmptyReply>(
                h.service, h.context, vtcontrol::CloseTabRequest { .tab = second->tab })
                .has_value());
    CHECK(h.host.sessionCount() == 1);
    CHECK(h.host.model().workspace(h.host.workspaceId())->tabCount() == 1);
    // A close the API asked for is a requested one, which a reboot restore does not bring back.
    CHECK(ends.causes == std::vector<vthost::SessionEnd>(3, vthost::SessionEnd::Requested));
    h.host.unsubscribeStream(&ends);
}

namespace
{
/// A call that may answer later.
template <typename Reply>
struct Pending
{
    std::optional<std::expected<Reply, ControlError>> result;
};

template <typename Reply, typename Request>
[[nodiscard]] std::shared_ptr<Pending<Reply>> callLater(ControlService& service,
                                                        vtcontrol::CallContext& context,
                                                        Request const& request)
{
    auto pending = std::make_shared<Pending<Reply>>();
    service.handle(context, request, [pending](std::expected<Reply, ControlError> reply) {
        REQUIRE_FALSE(pending->result.has_value());
        pending->result = std::move(reply);
    });
    return pending;
}

/// @return The payloads of type @p T the sink was handed.
template <typename T>
[[nodiscard]] std::vector<T> delivered(vtcontrol::testing::RecordingEventSink const& sink)
{
    auto found = std::vector<T> {};
    for (auto const& delivery: sink.deliveries)
        if (auto const* payload = std::get_if<T>(&delivery.event.payload))
            found.push_back(*payload);
    return found;
}

[[nodiscard]] uint32_t subscribe(ControlHarness& h, vtcontrol::SubscribeRequest const& request = {})
{
    auto const reply = callSync<vtcontrol::SubscribeReply>(h.service, h.context, request);
    REQUIRE(reply.has_value());
    return reply->subscription;
}
} // namespace

TEST_CASE("a subscriber receives block.finished with the exit code", "[vthost][control][events]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    auto const subscription = subscribe(h);

    h.terminal(session).writeToScreen(commandCycle("make", "out", 2));
    h.drain();

    auto const finished = delivered<vtcontrol::BlockFinishedEvent>(h.context.sink);
    REQUIRE(finished.size() == 1);
    CHECK(finished[0].block.exitCode == 2);
    CHECK(finished[0].block.command == "make");
    CHECK(finished[0].block.state == vtcontrol::BlockState::Finished);
    auto const& delivery = *std::ranges::find_if(h.context.sink.deliveries, [](auto const& d) {
        return std::holds_alternative<vtcontrol::BlockFinishedEvent>(d.event.payload);
    });
    CHECK(delivery.subscription == subscription);
    CHECK(delivery.event.origin.pane == h.paneId(session));
    CHECK(delivery.event.origin.tab.starts_with("tab:"));
    CHECK(delivery.event.origin.workspace.starts_with("workspace:"));
}

TEST_CASE("events filter by kind and by pane", "[vthost][control][events]")
{
    auto h = ControlHarness {};
    auto const first = h.openTab();
    auto const second = h.openTab();

    SECTION("by kind")
    {
        std::ignore = subscribe(h, vtcontrol::SubscribeRequest { .kinds = { "pane.bell" }, .pane = {} });
        h.terminal(first).writeToScreen(commandCycle("make", "out", 0));
        h.terminal(first).writeToScreen("\a");
        h.drain();
        REQUIRE(h.context.sink.deliveries.size() == 1);
        CHECK(std::holds_alternative<vtcontrol::BellEvent>(h.context.sink.deliveries[0].event.payload));
    }

    SECTION("by pane")
    {
        std::ignore = subscribe(h, vtcontrol::SubscribeRequest { .kinds = {}, .pane = h.paneId(second) });
        h.terminal(first).writeToScreen("\a");
        h.terminal(second).writeToScreen("\033]2;two\007");
        h.drain();
        REQUIRE(h.context.sink.deliveries.size() == 1);
        CHECK(delivered<vtcontrol::TitleChangedEvent>(h.context.sink).at(0).title == "two");
    }

    SECTION("an unknown kind is refused")
    {
        auto const reply = callSync<vtcontrol::SubscribeReply>(
            h.service, h.context, vtcontrol::SubscribeRequest { .kinds = { "pane.teleported" }, .pane = {} });
        REQUIRE_FALSE(reply.has_value());
        CHECK(reply.error().code == ControlErrorCode::BadRequest);
    }
}

TEST_CASE("a subscription takes only the events its grants allow", "[vthost][control][events][grants]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.context.granted = vtcontrol::ControlGrants::of({ vtcontrol::Permission::Read });

    SECTION("a subscribe naming only screen-level kinds is refused")
    {
        auto const reply = callSync<vtcontrol::SubscribeReply>(
            h.service, h.context, vtcontrol::SubscribeRequest { .kinds = { "block.finished" }, .pane = {} });
        REQUIRE_FALSE(reply.has_value());
        CHECK(reply.error().code == ControlErrorCode::Forbidden);
        CHECK(reply.error().message.contains("'screen'"));
    }

    SECTION("every kind, or a mix, delivers the granted ones")
    {
        auto const request =
            GENERATE(vtcontrol::SubscribeRequest {},
                     vtcontrol::SubscribeRequest { .kinds = { "block.finished", "pane.bell" }, .pane = {} });
        std::ignore = subscribe(h, request);
        h.terminal(session).writeToScreen(commandCycle("make secret", "out", 0));
        h.terminal(session).writeToScreen("\a");
        h.drain();
        CHECK(delivered<vtcontrol::BlockFinishedEvent>(h.context.sink).empty());
        CHECK(delivered<vtcontrol::BellEvent>(h.context.sink).size() == 1);
    }

    SECTION("with screen, block.finished is delivered")
    {
        h.context.granted = h.context.granted.with(vtcontrol::Permission::Screen);
        std::ignore = subscribe(h);
        h.terminal(session).writeToScreen(commandCycle("make", "out", 0));
        h.drain();
        CHECK(delivered<vtcontrol::BlockFinishedEvent>(h.context.sink).size() == 1);
    }
}

TEST_CASE("the layout and the working directory are events", "[vthost][control][events]")
{
    auto h = ControlHarness {};
    std::ignore = subscribe(h);
    auto const session = h.openTab();
    CHECK(delivered<vtcontrol::TabCreatedEvent>(h.context.sink).size() == 1);
    REQUIRE(delivered<vtcontrol::PaneCreatedEvent>(h.context.sink).size() == 1);
    CHECK(h.context.sink.deliveries.back().event.origin.pane == h.paneId(session));

    auto const split = callSync<vtcontrol::SplitReply>(h.service, h.context, vtcontrol::SplitRequest {});
    REQUIRE(split.has_value());
    REQUIRE(delivered<vtcontrol::PaneCreatedEvent>(h.context.sink).size() == 2);
    CHECK(h.context.sink.deliveries.back().event.origin.pane == split->pane);

    vthost::testing::reportDirectory(h.terminal(session), "/srv");
    h.drain();
    REQUIRE(delivered<vtcontrol::CwdChangedEvent>(h.context.sink).size() == 1);
    CHECK(delivered<vtcontrol::CwdChangedEvent>(h.context.sink)[0].cwd == "/srv");

    auto const tabId = h.context.sink.deliveries.front().event.origin.tab;
    REQUIRE_FALSE(tabId.empty());
    REQUIRE(callSync<vtcontrol::EmptyReply>(h.service, h.context, vtcontrol::CloseTabRequest {}).has_value());
    CHECK(delivered<vtcontrol::PaneClosedEvent>(h.context.sink).size() == 2);
    CHECK(delivered<vtcontrol::TabClosedEvent>(h.context.sink).size() == 1);
    // A closed pane still names the tab and workspace it was in.
    for (auto const& delivery: h.context.sink.deliveries)
        if (std::holds_alternative<vtcontrol::PaneClosedEvent>(delivery.event.payload))
        {
            CHECK(delivery.event.origin.tab == tabId);
            CHECK(delivery.event.origin.workspace.starts_with("workspace:"));
        }
}

TEST_CASE("control subscribers see pane.programStatusChanged and ls shows it",
          "[vthost][control][programstatus]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    std::ignore = subscribe(h);

    h.terminal(session).writeToScreen("\033]7501;state=working:app=deploy:progress=10\033\\"
                                      "\033]7501;state=blocked:id=eu:kind=permission:msg=QXBwbHk/\033\\");
    h.drain();
    auto const events = delivered<vtcontrol::ProgramStatusChangedEvent>(h.context.sink);
    REQUIRE(events.size() == 2);
    CHECK(h.context.sink.deliveries.back().event.origin.pane == h.paneId(session));
    // Each event carries the whole set as it reads when the loop gets to it.
    CHECK(events.back().reason == vtcontrol::ProgramStatusReason::Report);
    REQUIRE(events.back().records.size() == 2);
    auto const& blocked = events.back().records.back();
    CHECK(blocked.id == "eu");
    CHECK(blocked.state == vtcontrol::ProgramState::Blocked);
    CHECK(blocked.kind == vtcontrol::BlockedKind::Permission);
    CHECK(blocked.app == "deploy"); // inherited from the root
    CHECK(blocked.message == "Apply?");

    auto const list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    REQUIRE(list.has_value());
    auto const& pane = list->workspaces.at(0).tabs.at(0).panes.at(0);
    REQUIRE(pane.programStatus.size() == 2);
    CHECK(pane.programStatus.front().state == vtcontrol::ProgramState::Working);
    CHECK(pane.programStatus.front().progress == uint8_t { 10 });

    // A prompt drops both, and says why.
    h.terminal(session).writeToScreen("\033]133;A\007");
    h.drain();
    REQUIRE(delivered<vtcontrol::ProgramStatusChangedEvent>(h.context.sink).size() == 3);
    CHECK(delivered<vtcontrol::ProgramStatusChangedEvent>(h.context.sink).back().reason
          == vtcontrol::ProgramStatusReason::Prompt);
    CHECK(delivered<vtcontrol::ProgramStatusChangedEvent>(h.context.sink).back().records.empty());
}

TEST_CASE("program status needs screen, in its event as in ls", "[vthost][control][programstatus][grants]")
{
    // A record names the program and what it is doing, as a command line does: `read` lists the pane, not it.
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.context.granted = vtcontrol::ControlGrants::of({ vtcontrol::Permission::Read });
    auto const report = [&] {
        h.terminal(session).writeToScreen(
            "\033]7501;state=blocked:app=deploy:kind=permission:msg=QXBwbHk/\033\\");
        h.drain();
    };
    auto const listedRecords = [&] {
        auto const list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
        REQUIRE(list.has_value());
        return list->workspaces.at(0).tabs.at(0).panes.at(0).programStatus;
    };

    SECTION("read alone: refused by name, left out of every kind and of ls")
    {
        auto const refused = callSync<vtcontrol::SubscribeReply>(
            h.service,
            h.context,
            vtcontrol::SubscribeRequest { .kinds = { "pane.programStatusChanged" }, .pane = {} });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ControlErrorCode::Forbidden);
        CHECK(refused.error().message.contains("'screen'"));

        std::ignore = subscribe(h);
        report();
        CHECK(delivered<vtcontrol::ProgramStatusChangedEvent>(h.context.sink).empty());
        CHECK(listedRecords().empty());
    }

    SECTION("with screen, both carry it")
    {
        h.context.granted = h.context.granted.with(vtcontrol::Permission::Screen);
        std::ignore = subscribe(h);
        report();
        CHECK(delivered<vtcontrol::ProgramStatusChangedEvent>(h.context.sink).size() == 1);
        REQUIRE(listedRecords().size() == 1);
        CHECK(listedRecords().front().message == "Apply?");
    }
}

TEST_CASE("program status reported on another thread reaches the subscriber on the loop",
          "[vthost][control][programstatus][threading]")
{
    // The parser thread changes the set and raises the signal under the terminal lock; the host posts it, and
    // the loop thread -- this one -- reads the set back under the lock and publishes it.
    auto h = ControlHarness {};
    auto const session = h.openTab();
    std::ignore = subscribe(h);
    auto& terminal = h.terminal(session);
    auto parser = std::thread { [&terminal] {
        for (auto const index: std::views::iota(0, 50))
            terminal.writeToScreen(std::format("\033]7501;state=working:id=job{}\033\\", index));
    } };
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 10 };
    while (delivered<vtcontrol::ProgramStatusChangedEvent>(h.context.sink).size() < 50
           && std::chrono::steady_clock::now() < deadline)
        std::ignore = h.loop.runOnce(std::chrono::milliseconds { 1 });
    parser.join();
    h.drain();

    auto const events = delivered<vtcontrol::ProgramStatusChangedEvent>(h.context.sink);
    REQUIRE(events.size() == 50);
    CHECK(events.back().records.size() == 50);
}

TEST_CASE("unsubscribing an unknown subscription is NotFound", "[vthost][control][events]")
{
    auto h = ControlHarness {};
    auto const reply = callSync<vtcontrol::EmptyReply>(
        h.service, h.context, vtcontrol::UnsubscribeRequest { .subscription = 4711 });
    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().code == ControlErrorCode::NotFound);

    auto const subscription = subscribe(h);
    CHECK(callSync<vtcontrol::EmptyReply>(
              h.service, h.context, vtcontrol::UnsubscribeRequest { .subscription = subscription })
              .has_value());
    std::ignore = h.openTab();
    CHECK(h.context.sink.deliveries.empty());
}

TEST_CASE("block.wait completes when the block ends, at once when it already has", "[vthost][control][wait]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.terminal(session).writeToScreen("\033]133;A\007$ \033]133;B\007sleep 1\r\n\033]133;C\007");

    auto const list =
        callSync<vtcontrol::BlockListReply>(h.service, h.context, vtcontrol::BlockListRequest {});
    REQUIRE(list.has_value());
    REQUIRE(list->blocks.size() == 1);
    auto const block = list->blocks[0].id;
    CHECK(list->blocks[0].state == vtcontrol::BlockState::Running);

    auto const waiting = callLater<vtcontrol::WaitReply>(
        h.service, h.context, vtcontrol::WaitRequest { .block = block, .pane = {} });
    // And one by pane, which waits for the running command.
    auto const byPane = callLater<vtcontrol::WaitReply>(h.service, h.context, vtcontrol::WaitRequest {});
    CHECK_FALSE(waiting->result.has_value());
    CHECK_FALSE(byPane->result.has_value());

    h.terminal(session).writeToScreen("\033]133;D;0\007");
    h.drain();
    REQUIRE(waiting->result.has_value());
    REQUIRE(waiting->result->has_value());
    REQUIRE((*waiting->result)->block.has_value());
    CHECK((*waiting->result)->block->id == block);
    CHECK((*waiting->result)->block->exitCode == 0);
    REQUIRE(byPane->result.has_value());
    CHECK((*byPane->result)->block->id == block);

    auto const again = callSync<vtcontrol::WaitReply>(
        h.service, h.context, vtcontrol::WaitRequest { .block = block, .pane = {} });
    REQUIRE(again.has_value());
    CHECK(again->block->id == block);
}

TEST_CASE("block.wait on an idle pane returns the last finished block at once", "[vthost][control][wait]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();

    auto const none = callSync<vtcontrol::WaitReply>(h.service, h.context, vtcontrol::WaitRequest {});
    REQUIRE(none.has_value());
    CHECK_FALSE(none->block.has_value());

    h.terminal(session).writeToScreen(commandCycle("true", "", 0));
    h.terminal(session).writeToScreen("\033]133;A\007$ ");
    auto const last = callSync<vtcontrol::WaitReply>(h.service, h.context, vtcontrol::WaitRequest {});
    REQUIRE(last.has_value());
    REQUIRE(last->block.has_value());
    CHECK(last->block->command == "true");
}

TEST_CASE("send with wait commandFinished answers with that command's block", "[vthost][control][wait]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();

    SECTION("at the prompt")
    {
        h.terminal(session).writeToScreen("\033]133;A\007$ \033]133;B\007");
        auto const sent = callLater<vtcontrol::SendReply>(
            h.service,
            h.context,
            vtcontrol::SendRequest {
                .pane = {}, .text = "make\r", .wait = vtcontrol::SendWait::CommandFinished });
        CHECK_FALSE(sent->result.has_value());
        h.terminal(session).writeToScreen("make\r\n\033]133;C\007out\r\n\033]133;D;2\007");
        h.drain();
        REQUIRE(sent->result.has_value());
        REQUIRE(sent->result->has_value());
        REQUIRE((*sent->result)->block.has_value());
        CHECK((*sent->result)->block->exitCode == 2);
    }

    SECTION("while a command runs, the next one")
    {
        h.terminal(session).writeToScreen(commandCycle("first", "1", 0));
        h.terminal(session).writeToScreen("\033]133;A\007$ \033]133;B\007sleep\r\n\033]133;C\007");
        auto const sent = callLater<vtcontrol::SendReply>(
            h.service,
            h.context,
            vtcontrol::SendRequest {
                .pane = {}, .text = "make\r", .wait = vtcontrol::SendWait::CommandFinished });
        h.terminal(session).writeToScreen("\033]133;D;0\007");
        h.drain();
        CHECK_FALSE(sent->result.has_value());
        h.terminal(session).writeToScreen(commandCycle("make", "out", 5));
        h.drain();
        REQUIRE(sent->result.has_value());
        CHECK((*sent->result)->block->exitCode == 5);
        CHECK((*sent->result)->block->command == "make");
    }
}

TEST_CASE("a nested prompt supersedes a waited-on block", "[vthost][control][wait]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.terminal(session).writeToScreen("\033]133;A\007$ \033]133;B\007ssh box\r\n\033]133;C\007");
    auto const waiting = callLater<vtcontrol::WaitReply>(h.service, h.context, vtcontrol::WaitRequest {});
    CHECK_FALSE(waiting->result.has_value());

    // The remote shell's prompt, with no ;D for the ssh: the block ends implicitly.
    h.terminal(session).writeToScreen("\033]133;A\007remote$ ");
    h.drain();
    REQUIRE(waiting->result.has_value());
    REQUIRE(waiting->result->has_value());
    CHECK((*waiting->result)->block->state == vtcontrol::BlockState::Superseded);
}

TEST_CASE("a pane closing under a waiter answers NotFound", "[vthost][control][wait]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    h.terminal(session).writeToScreen("\033]133;A\007$ \033]133;B\007");
    auto const sent = callLater<vtcontrol::SendReply>(
        h.service,
        h.context,
        vtcontrol::SendRequest {
            .pane = {}, .text = "exit\r", .wait = vtcontrol::SendWait::CommandFinished });
    h.host.handleSessionExit(session);
    REQUIRE(sent->result.has_value());
    REQUIRE_FALSE(sent->result->has_value());
    CHECK(sent->result->error().code == ControlErrorCode::NotFound);
    CHECK(sent->result->error().message.contains("closed"));
}

TEST_CASE("sinkClosed drops a sink's subscriptions and waiters", "[vthost][control][events]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    std::ignore = subscribe(h);
    h.terminal(session).writeToScreen("\033]133;A\007$ \033]133;B\007make\r\n\033]133;C\007");
    auto const waiting = callLater<vtcontrol::WaitReply>(h.service, h.context, vtcontrol::WaitRequest {});
    h.context.sink.deliveries.clear();

    h.service.sinkClosed(h.context.sink);
    h.terminal(session).writeToScreen("\033]133;D;0\007\a");
    h.drain();
    CHECK(h.context.sink.deliveries.empty());
    CHECK_FALSE(waiting->result.has_value());
}

TEST_CASE("a connection's subscriptions and waits are capped, and freed as it closes",
          "[vthost][control][limits]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();

    SECTION("subscriptions")
    {
        for ([[maybe_unused]] auto const index:
             std::views::iota(std::size_t { 0 }, vthost::control::DefaultMaxSubscriptionsPerConnection))
            std::ignore = subscribe(h);
        auto const refused =
            callSync<vtcontrol::SubscribeReply>(h.service, h.context, vtcontrol::SubscribeRequest {});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ControlErrorCode::LimitReached);

        // Another connection has a share of its own.
        auto other = FakeCallContext {};
        CHECK(callSync<vtcontrol::SubscribeReply>(h.service, other, vtcontrol::SubscribeRequest {})
                  .has_value());
        h.service.sinkClosed(other.sink);

        h.service.sinkClosed(h.context.sink);
        CHECK(callSync<vtcontrol::SubscribeReply>(h.service, h.context, vtcontrol::SubscribeRequest {})
                  .has_value());
    }

    SECTION("waits")
    {
        // A command runs: every wait below is pending until it ends.
        h.terminal(session).writeToScreen("\033]133;A\007$ \033]133;B\007make\r\n\033]133;C\007");
        auto pending = 0;
        for ([[maybe_unused]] auto const index:
             std::views::iota(std::size_t { 0 }, vthost::control::DefaultMaxWaitsPerConnection))
            h.service.handle(h.context, vtcontrol::WaitRequest {}, [&](auto) { ++pending; });
        CHECK(pending == 0);

        auto const block = callSync<vtcontrol::WaitReply>(h.service, h.context, vtcontrol::WaitRequest {});
        REQUIRE_FALSE(block.has_value());
        CHECK(block.error().code == ControlErrorCode::LimitReached);

        // A send that cannot wait types nothing.
        auto const refused = callSync<vtcontrol::SendReply>(
            h.service,
            h.context,
            vtcontrol::SendRequest {
                .pane = {}, .text = "typed\n", .wait = vtcontrol::SendWait::CommandFinished });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ControlErrorCode::LimitReached);
        auto& tapped = dynamic_cast<vthost::TappingPty&>(h.terminal(session).device());
        CHECK(dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer().empty());

        // Closing the connection frees its share.
        h.service.sinkClosed(h.context.sink);
        auto answered = std::optional<std::expected<vtcontrol::WaitReply, ControlError>> {};
        h.service.handle(
            h.context, vtcontrol::WaitRequest {}, [&](auto reply) { answered = std::move(reply); });
        CHECK_FALSE(answered.has_value());
        h.terminal(session).writeToScreen("\033]133;D;0\007");
        h.drain();
        REQUIRE(answered.has_value());
        CHECK(answered->has_value());
        CHECK(pending == 0);
    }
}

TEST_CASE("client attach and detach are events", "[vthost][control][events]")
{
    auto h = ControlHarness {};
    std::ignore = subscribe(
        h, vtcontrol::SubscribeRequest { .kinds = { "client.attached", "client.detached" }, .pane = {} });
    h.service.clientAttached("7");
    h.service.clientDetached("7");
    REQUIRE(h.context.sink.deliveries.size() == 2);
    CHECK(delivered<vtcontrol::ClientAttachedEvent>(h.context.sink).at(0).client == "7");
    CHECK(delivered<vtcontrol::ClientDetachedEvent>(h.context.sink).at(0).client == "7");
}

TEST_CASE("a block finished on another thread reaches the subscriber on the loop",
          "[vthost][control][events][threading]")
{
    // The parser thread raises the finish; the host posts it, and only the loop thread -- this one -- touches
    // the subscriptions and the waiters.
    auto h = ControlHarness {};
    auto const session = h.openTab();
    std::ignore = subscribe(h);
    h.terminal(session).writeToScreen("\033]133;A\007$ \033]133;B\007make\r\n\033]133;C\007");
    auto const waiting = callLater<vtcontrol::WaitReply>(h.service, h.context, vtcontrol::WaitRequest {});

    auto& terminal = h.terminal(session);
    auto parser = std::thread { [&terminal] { terminal.writeToScreen("out\r\n\033]133;D;4\007"); } };
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 10 };
    while (!waiting->result && std::chrono::steady_clock::now() < deadline)
        std::ignore = h.loop.runOnce(std::chrono::milliseconds { 1 });
    parser.join();
    h.drain();

    REQUIRE(waiting->result.has_value());
    CHECK((*waiting->result)->block->exitCode == 4);
    CHECK(delivered<vtcontrol::BlockFinishedEvent>(h.context.sink).size() == 1);
}

namespace
{

/// Whether a test window offered Capability::WorkspaceSwitch.
enum class SwitchSupport : uint8_t
{
    Absent = 0,
    Offered,
};

/// An attached window that shows the workspaces it is given, can (or cannot) be asked to switch, and
/// records what it was asked.
class FakeWindow final: public vthost::SessionStreamEvents
{
  public:
    FakeWindow(std::uint64_t index, SwitchSupport support):
        _id { .endpoint = "native", .index = index }, _support { support }
    {
    }

    [[nodiscard]] vthost::ConnectionId connectionId() const override { return _id; }
    [[nodiscard]] bool canSwitch() const noexcept override { return _support == SwitchSupport::Offered; }
    [[nodiscard]] bool follows(vtworkspace::WorkspaceId workspace) const noexcept override
    {
        return workspace == shown;
    }
    void requestSwitch(vthost::proto::SwitchRequest const& request) override { asked.push_back(request); }

    vtworkspace::WorkspaceId shown {};
    std::vector<vthost::proto::SwitchRequest> asked;

  private:
    vthost::ConnectionId _id;
    SwitchSupport _support;
};

/// What a `workspace.switch` call answered with, once it has.
using SwitchAnswer = std::optional<std::expected<vtcontrol::SwitchWorkspaceReply, ControlError>>;

void callSwitch(ControlHarness& h, vtcontrol::SwitchWorkspaceRequest const& request, SwitchAnswer& answer)
{
    h.service.handle(
        h.context, request, [&answer](std::expected<vtcontrol::SwitchWorkspaceReply, ControlError> reply) {
            answer = std::move(reply);
        });
}

} // namespace

TEST_CASE("workspace.list reports a workspace's visibility, clients and native id",
          "[vthost][control][switch]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto const workspace = h.host.workspaceId();
    auto window = FakeWindow { 1, SwitchSupport::Offered };
    h.host.noteClientAttached(&window);

    auto list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    REQUIRE(list.has_value());
    REQUIRE(list->workspaces.size() == 1);
    CHECK(list->workspaces[0].native == std::to_string(workspace.value));
    CHECK(list->workspaces[0].visibility == "left");
    CHECK(list->workspaces[0].clients == 0);

    window.shown = workspace;
    list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    CHECK(list->workspaces[0].visibility == "shown");
    CHECK(list->workspaces[0].clients == 1);

    window.shown = {};
    h.host.noteWorkspaceReleased(workspace, vthost::proto::WorkspaceRelease::SwitchedAway);
    list = callSync<vtcontrol::ListReply>(h.service, h.context, vtcontrol::ListRequest {});
    CHECK(list->workspaces[0].visibility == "hidden");
    h.host.noteClientDetached(&window);
}

TEST_CASE("workspace.switch asks the window showing the workspace and relays its answer",
          "[vthost][control][switch]")
{
    using vthost::proto::SwitchOutcome;
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto window = FakeWindow { 1, SwitchSupport::Offered };
    window.shown = h.host.workspaceId();
    h.host.noteClientAttached(&window);
    auto answer = SwitchAnswer {};

    callSwitch(h, { .toLabel = "admin", .toWorkspace = "build" }, answer);
    REQUIRE(window.asked.size() == 1);
    CHECK(window.asked.front().target.kind == vthost::proto::SwitchDaemonKind::Label);
    CHECK(window.asked.front().target.daemon == "admin");
    CHECK(window.asked.front().target.workspace == "build");
    CHECK(window.asked.front().workspace == h.host.workspaceId().value);
    CHECK_FALSE(answer.has_value());

    h.host.completeSwitch({ .endpoint = "native", .index = 1 },
                          { .request = window.asked.front().request, .outcome = SwitchOutcome::Switched });
    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    CHECK((*answer)->outcome == "switched");

    // Every outcome has its name; a host target is read as one.
    struct Row
    {
        SwitchOutcome outcome;
        std::string_view name;
    };
    for (auto const& [outcome, name]: {
             Row { SwitchOutcome::FocusedExisting, "focused" },
             Row { SwitchOutcome::PickerShown, "picker" },
             Row { SwitchOutcome::Refused, "refused" },
             Row { SwitchOutcome::Failed, "failed" },
         })
    {
        answer.reset();
        callSwitch(h, { .toHost = "ssh://me@box" }, answer);
        REQUIRE(window.asked.back().target.kind == vthost::proto::SwitchDaemonKind::Host);
        CHECK(window.asked.back().target.daemon == "ssh://me@box");
        h.host.completeSwitch(
            { .endpoint = "native", .index = 1 },
            { .request = window.asked.back().request, .outcome = outcome, .detail = "why" });
        REQUIRE(answer.has_value());
        REQUIRE(answer->has_value());
        CHECK((*answer)->outcome == name);
        CHECK((*answer)->detail == "why");
    }
    h.host.noteClientDetached(&window);
}

TEST_CASE("workspace.switch fails for a window that did not offer the capability",
          "[vthost][control][switch]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto window = FakeWindow { 1, SwitchSupport::Absent };
    window.shown = h.host.workspaceId();
    h.host.noteClientAttached(&window);
    auto answer = SwitchAnswer {};

    callSwitch(h, { .toLabel = "admin" }, answer);

    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    CHECK((*answer)->outcome == "failed");
    CHECK((*answer)->detail == "the window cannot switch (client too old)");
    CHECK(window.asked.empty());
    h.host.noteClientDetached(&window);
}

TEST_CASE("workspace.switch errors: no window, several windows, both targets", "[vthost][control][switch]")
{
    auto h = ControlHarness {};
    auto const session = h.openTab();
    auto const workspace = h.host.workspaceId();
    auto answer = SwitchAnswer {};

    SECTION("no client shows the workspace")
    {
        callSwitch(h, { .toLabel = "admin" }, answer);
        REQUIRE(answer.has_value());
        REQUIRE_FALSE(answer->has_value());
        CHECK((*answer).error().code == ControlErrorCode::NotFound);
        CHECK((*answer).error().message.starts_with("no client shows workspace"));
    }
    SECTION("several windows and no typist are ambiguous, listing them")
    {
        auto first = FakeWindow { 1, SwitchSupport::Offered };
        auto second = FakeWindow { 2, SwitchSupport::Offered };
        first.shown = workspace;
        second.shown = workspace;
        h.host.noteClientAttached(&first);
        h.host.noteClientAttached(&second);

        callSwitch(h, { .toLabel = "admin" }, answer);
        REQUIRE(answer.has_value());
        REQUIRE_FALSE(answer->has_value());
        CHECK((*answer).error().code == ControlErrorCode::Ambiguous);
        CHECK((*answer).error().candidates == std::vector<std::string> { "native#1", "native#2" });
        CHECK(first.asked.empty());
        CHECK(second.asked.empty());

        // The one that typed last is the one asked.
        h.host.writeInputFrom(session, "x", { .endpoint = "native", .index = 2 });
        answer.reset();
        callSwitch(h, { .toLabel = "admin" }, answer);
        CHECK(first.asked.empty());
        CHECK(second.asked.size() == 1);
        h.host.noteClientDetached(&first);
        h.host.noteClientDetached(&second);
    }
    SECTION("a label and a host together are a bad request")
    {
        callSwitch(h, { .toLabel = "admin", .toHost = "box" }, answer);
        REQUIRE(answer.has_value());
        REQUIRE_FALSE(answer->has_value());
        CHECK((*answer).error().code == ControlErrorCode::BadRequest);
    }
    SECTION("an unknown workspace is not found")
    {
        callSwitch(h, { .workspace = "nope", .toLabel = "admin" }, answer);
        REQUIRE(answer.has_value());
        REQUIRE_FALSE(answer->has_value());
        CHECK((*answer).error().code == ControlErrorCode::NotFound);
    }
}

TEST_CASE("workspace.switch drops the answer of a connection that closed", "[vthost][control][switch]")
{
    auto h = ControlHarness {};
    std::ignore = h.openTab();
    auto window = FakeWindow { 1, SwitchSupport::Offered };
    window.shown = h.host.workspaceId();
    h.host.noteClientAttached(&window);
    auto answer = SwitchAnswer {};
    callSwitch(h, { .toLabel = "admin" }, answer);
    REQUIRE(window.asked.size() == 1);

    h.service.sinkClosed(h.context.events());
    h.host.completeSwitch({ .endpoint = "native", .index = 1 }, { .request = window.asked.front().request });

    CHECK_FALSE(answer.has_value());
    h.host.noteClientDetached(&window);
}
