// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Base64.hpp>
#include <core/Random.hpp>
#include <core/Utf8Path.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Tls.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SoftwareSecurityKey.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Events.hpp>
#include <vtcontrol/Json.hpp>
#include <vtcontrol/LegacySpelling.hpp>
#include <vtcontrol/testing/FakeControlApi.hpp>
#include <vtcontrol/testing/LegacyShapes.hpp>
#include <vthost/CommandBlockWire.hpp>
#include <vthost/DaemonControlWire.hpp>
#include <vthost/FlushPolicy.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/KeyAuth.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/NodeMove.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshot.hpp>
#include <vthost/TabMerge.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/WorkspaceGit.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/control/ControlService.hpp>
#include <vthost/proto/ControlVersions.hpp>
#include <vthost/proto/PduTrace.hpp>
#include <vthost/testing/FakeDaemonControl.hpp>
#include <vthost/testing/FakeExtensionHosting.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/NativeSessionHarness.hpp>
#include <vthost/testing/PreNegotiationCodec.hpp>
#include <vthost/testing/ShippedCodec.hpp>
#include <vthost/testing/TestHelpers.hpp>
#include <vtworkspace/LayoutTree.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using vthost::NativeSession;
using vthost::NativeSessionFlushTester;
using vthost::testing::AttentionFixture;
using vthost::testing::cellOf;
using vthost::testing::countingPtyOf;
using vthost::testing::countingPtys;
using vthost::testing::FlushClient;
using vthost::testing::FlushHarness;
using vthost::testing::HarnessOptions;
using vthost::testing::incrementCount;
using vthost::testing::inputRequest;
using vthost::testing::layoutWorkspaces;
using vthost::testing::MetaFixture;
using vthost::testing::narrowTokenOptions;
using vthost::testing::NativeHarness;
using vthost::testing::newSession;
using vthost::testing::recordingSpawnDirectories;
using vthost::testing::serverHelloOf;
using vthost::testing::sessionsOf;
using vthost::testing::snapshotSessions;
using vthost::testing::statedDirectory;
using vthost::testing::SummaryFixture;
using vthost::testing::turnUntil;
namespace proto = vthost::proto;

// {{{ observer connections
namespace
{

/// What an ObserverFixture's host is built with.
struct ObserverOptions
{
    /// The status-line extensions the hosted sessions run; none by default.
    std::shared_ptr<vthost::IExtensionHosting> extensions {};
    /// What makes each session's PTY; unset, a MockPty.
    vthost::PtyFactory ptyFactory {};
    /// What the host restores as it starts; nothing by default.
    std::optional<vthost::WorkspaceRestore> restore = std::nullopt;
};

/// Two workspaces of one session each, the fakes the census and the control API are told through, and
/// connections handshaken by hand as observers or as windows.
struct ObserverFixture
{
    /// A constructor, not an aggregate's member initializers copying sibling members into a designated
    /// FlushHarness: MSVC 19.5x's optimizer crashes (C1001) on the latter.
    /// @param options What the host is built with.
    explicit ObserverFixture(ObserverOptions options = {}):
        flush { .services = { .daemon = daemon, .control = control },
                .extensions = std::move(options.extensions),
                .ptyFactory = std::move(options.ptyFactory),
                .restore = std::move(options.restore) }
    {
    }

    vthost::testing::FakeDaemonControl daemon {};
    vtcontrol::testing::FakeControlApi control {};
    FlushHarness flush;
    vtworkspace::SessionId inFirst = newSession(flush.h.host);
    uint64_t first = flush.h.host.workspaceId().value;
    uint64_t second = flush.h.host.createWorkspace()->id().value;
    vtworkspace::SessionId inSecond { *sessionsOf(flush.h.host, second).begin() };

    /// @return An observer's hello, sharing what an observer needs and what its sidebar edits with.
    [[nodiscard]] static proto::ClientHello observerHello()
    {
        return proto::ClientHello { .capabilities =
                                        proto::CapabilitySet::of({ proto::Capability::DaemonFollow,
                                                                   proto::Capability::WorkspaceMeta,
                                                                   proto::Capability::WorkspaceColor,
                                                                   proto::Capability::DaemonAttention,
                                                                   proto::Capability::TabEdits,
                                                                   proto::Capability::ColorPreference,
                                                                   proto::Capability::CellPixelSize,
                                                                   proto::Capability::InitialClientArea,
                                                                   proto::Capability::ClipboardTargets,
                                                                   proto::Capability::Observe }),
                                    .role = proto::ConnectionRole::Observe };
    }

    /// @return An observer's hello that does not share @p missing.
    [[nodiscard]] static proto::ClientHello observerHelloWithout(proto::Capability missing)
    {
        auto hello = observerHello();
        auto const ident = static_cast<uint64_t>(missing);
        auto& idents = hello.capabilities.idents;
        idents.erase(std::ranges::remove(idents, ident).begin(), idents.end());
        return hello;
    }

    /// @return An observer's hello that also batches its layouts, as a window's does: a promotion is answered
    ///         by the batch.
    [[nodiscard]] static proto::ClientHello promotableHello()
    {
        auto hello = observerHello();
        for (auto const capability: { proto::Capability::LayoutBatch,
                                      proto::Capability::ProgramStatus,
                                      proto::Capability::ExtensionPermissions })
            hello.capabilities.idents.push_back(std::to_underlying(capability));
        return hello;
    }

    /// Has @p client ask for @p workspace under @p serial, and lets what it is sent go out.
    void select(FlushClient& client, uint64_t workspace, uint64_t serial = 5)
    {
        NativeSessionFlushTester::receive(
            *client.session, proto::SelectWorkspace { .workspace = workspace }, serial);
        settle();
    }

    /// Has a new observer ask to be promoted to a workspace made for @p token, and lets what it is sent go
    /// out.
    /// @return The workspace the observer selects now; 0 for none.
    uint64_t promoteToNew(proto::CreationToken const& token)
    {
        auto& client = observe(promotableHello());
        NativeSessionFlushTester::receive(
            *client.session, proto::SelectWorkspace { .workspace = 0, .creationToken = token }, 5);
        settle();
        auto const ends = batchEndsOf(client);
        auto const answer = std::ranges::find(ends, uint64_t { 5 }, &proto::LayoutBatchEnd::causeSerial);
        REQUIRE(answer != ends.end());
        return answer->selected;
    }

    /// @return A creation token unlike any other @p seed names.
    [[nodiscard]] static proto::CreationToken tokenOf(uint8_t seed)
    {
        auto token = proto::CreationToken {};
        token.fill(std::byte { seed });
        return token;
    }

    /// Has @p client ask to observe only, and lets what it is sent go out.
    void demote(FlushClient& client)
    {
        NativeSessionFlushTester::receive(*client.session, proto::ObserveOnly {}, 6);
        settle();
    }

    /// @return The LayoutBatchEnds @p client received after its first @p skip frames.
    [[nodiscard]] static std::vector<proto::LayoutBatchEnd> batchEndsOf(FlushClient const& client,
                                                                        std::size_t skip = 0)
    {
        auto ends = std::vector<proto::LayoutBatchEnd> {};
        for (auto const& frame: client.received | std::views::drop(skip))
            if (auto const* end = std::get_if<proto::LayoutBatchEnd>(&frame.pdu))
                ends.push_back(*end);
        return ends;
    }

    /// Connects an observer saying @p hello, and lets what it is sent go out.
    FlushClient& observe(proto::ClientHello hello = observerHello())
    {
        auto& client = flush.connect();
        NativeSessionFlushTester::handshake(*client.session, std::move(hello));
        settle();
        return client;
    }

    /// Attaches a window following the daemon that selects @p workspace.
    FlushClient& attachSelecting(uint64_t workspace)
    {
        return flush.attach(workspace == first ? inFirst : inSecond,
                            SummaryFixture::helloSelecting(workspace));
    }

    /// Attaches a window that selects @p workspace and could observe instead: it shares what an observer
    /// must, and what a demotion is told by.
    FlushClient& attachDemotable(uint64_t workspace)
    {
        auto hello = SummaryFixture::helloSelecting(workspace);
        for (auto const capability: { proto::Capability::Observe,
                                      proto::Capability::WorkspaceMeta,
                                      proto::Capability::DaemonAttention,
                                      proto::Capability::LayoutBatch })
            hello.capabilities.idents.push_back(std::to_underlying(capability));
        return flush.attach(workspace == first ? inFirst : inSecond, std::move(hello));
    }

    /// Lets what is due go out, and the clients read it.
    void settle() { flush.runDueFlushes(); }

    /// @return The PDU types @p client was answered Unsupported for, after its first @p skip frames.
    [[nodiscard]] static std::vector<uint64_t> refusedBy(FlushClient const& client, std::size_t skip = 0)
    {
        auto refused = std::vector<uint64_t> {};
        for (auto const& frame: client.received | std::views::drop(skip))
            if (auto const* unsupported = std::get_if<proto::Unsupported>(&frame.pdu))
                refused.push_back(unsupported->ident);
        return refused;
    }
};

} // namespace

TEST_CASE("a narrow-token Observe hello is refused like an attach", "[vthost][native][grants][observer]")
{
    // The narrow tokens, over an endpoint that offers what an observer needs.
    auto const options = [] {
        auto narrow = narrowTokenOptions();
        narrow.services.handshake.capabilities = vthost::HandshakePolicy {}.capabilities;
        return narrow;
    };
    auto const answerTo = [&](std::string token) {
        auto h = NativeHarness { options() };
        auto hello = ObserverFixture::observerHello();
        hello.token = std::move(token);
        auto const got = h.exchange({ std::move(hello) }, 1);
        CHECK(h.session->phase() == NativeSession::ConnectionPhase::AwaitingHello);
        CHECK(h.host.connectedObservers() == 0);
        REQUIRE(got.size() == 1);
        auto bytes = proto::Writer {};
        proto::encodePdu(bytes, got[0].serial, got[0].pdu);
        return std::vector<std::byte> { bytes.view().begin(), bytes.view().end() };
    };
    CHECK(answerTo("ro") == answerTo("wrong"));
    CHECK(answerTo("arranger") == answerTo("wrong"));

    // The full token observes.
    auto h = NativeHarness { options() };
    auto hello = ObserverFixture::observerHello();
    hello.token = "admin";
    std::ignore = h.exchange({ std::move(hello) }, 1);
    CHECK(h.session->phase() == NativeSession::ConnectionPhase::Observing);
}

TEST_CASE("an observer without daemon-follow or workspace-meta is refused", "[vthost][native][observer]")
{
    auto const missing = GENERATE(proto::Capability::DaemonFollow, proto::Capability::WorkspaceMeta);
    auto f = ObserverFixture {};
    auto& client = f.flush.connect();
    auto const accepted = NativeSessionFlushTester::tryHandshake(
        *client.session, ObserverFixture::observerHelloWithout(missing));
    CHECK_FALSE(accepted);
    f.settle();
    CHECK(client.session->phase() == NativeSession::ConnectionPhase::AwaitingHello);
    CHECK(f.flush.h.host.connectedObservers() == 0);
    // Answered as a refusal: a ServerHello naming no capability, and no layout.
    CHECK(std::ranges::none_of(client.received, [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::LayoutState>(frame.pdu);
    }));
}

TEST_CASE("an Observe hello that does not share observe is answered as a Control connection",
          "[vthost][native][observer][compat]")
{
    auto f = ObserverFixture {};
    auto hello = ObserverFixture::observerHelloWithout(proto::Capability::Observe);
    auto& client = f.flush.connect();
    auto const accepted = NativeSessionFlushTester::tryHandshake(*client.session, std::move(hello));
    REQUIRE(accepted);
    f.settle();

    // As a daemon that predates the role reads it: told nothing, counted as no one.
    CHECK(client.session->phase() == NativeSession::ConnectionPhase::Controlling);
    CHECK(f.flush.h.host.connectedObservers() == 0);
    CHECK(f.flush.h.host.connectedClients() == 0);
    CHECK(std::ranges::none_of(client.received, [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::LayoutState>(frame.pdu)
               || std::holds_alternative<proto::PaneSummary>(frame.pdu);
    }));
}

TEST_CASE("an observer's Input and ResizePane are refused", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& observer = f.observe();
    auto const* terminal = f.flush.h.host.terminal(f.inFirst);
    auto const pageBefore = terminal->pageSize();

    NativeSessionFlushTester::receive(*observer.session, inputRequest(f.inFirst.value, "ls\r"), 7);
    NativeSessionFlushTester::receive(*observer.session,
                                      proto::ResizePane { .session = f.inFirst.value,
                                                          .columns = unbox<uint32_t>(pageBefore.columns) + 3,
                                                          .lines = unbox<uint32_t>(pageBefore.lines) + 2 },
                                      8);
    NativeSessionFlushTester::receive(*observer.session, proto::CreateTab {}, 9);
    f.settle();

    auto& tapped = dynamic_cast<vthost::TappingPty&>(f.flush.h.host.terminal(f.inFirst)->device());
    CHECK(dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer().empty());
    CHECK(f.flush.h.host.terminal(f.inFirst)->pageSize() == pageBefore);
    CHECK(f.flush.h.host.sessionIds().size() == 2);
    CHECK(ObserverFixture::refusedBy(observer)
          == std::vector<uint64_t> { std::to_underlying(proto::PduType::Input),
                                     std::to_underlying(proto::PduType::ResizePane),
                                     std::to_underlying(proto::PduType::CreateTab) });
}

TEST_CASE("an observer's ResizeRequest, hello area and colour change nothing until promotion",
          "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto const pageBefore = host.terminal(f.inFirst)->pageSize();
    auto const schemeBefore = host.defaultColorPreference();
    auto const otherScheme = schemeBefore == vtbackend::ColorPreference::Dark
                                 ? proto::WireColorPreference::Light
                                 : proto::WireColorPreference::Dark;
    auto hello = ObserverFixture::observerHello();
    hello.clientArea = proto::WireClientArea { .columns = 150, .lines = 50 };
    hello.cellPixelSize = proto::WireCellPixelSize { .width = 11, .height = 23 };
    hello.colorPreference = otherScheme;
    auto& observer = f.observe(std::move(hello));
    CHECK(host.terminal(f.inFirst)->pageSize() == pageBefore);
    CHECK(host.defaultColorPreference() == schemeBefore);

    NativeSessionFlushTester::receive(*observer.session,
                                      proto::ResizeRequest { .columns = 120, .lines = 40 });
    f.settle();

    CHECK(host.terminal(f.inFirst)->pageSize() == pageBefore);
    CHECK(host.terminal(f.inSecond)->pageSize() == pageBefore);
    CHECK(ObserverFixture::refusedBy(observer).empty());
    // Held for the viewer it may become.
    CHECK(NativeSessionFlushTester::heldArea(*observer.session)
          == vtpty::PageSize { vtpty::LineCount(40), vtpty::ColumnCount(120) });
}

TEST_CASE("an observer renames, colours and closes a workspace and closes a pane",
          "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto const third = host.createWorkspace()->id().value;
    auto const inThird = vtworkspace::SessionId { *sessionsOf(host, third).begin() };
    auto& observer = f.observe();
    auto const ref = vtcontrol::ObjectRef {
        .kind = vtcontrol::ObjectKind::Workspace,
        .uuid = *host.objects().uuidOf(vtworkspace::WorkspaceId { f.second }),
    };

    NativeSessionFlushTester::receive(*observer.session,
                                      proto::SetWorkspaceName { .workspace = f.second, .name = "ops" });
    NativeSessionFlushTester::receive(
        *observer.session,
        proto::SetWorkspaceColor { .workspace = f.second,
                                   .color = proto::WireWorkspaceColor { .rgb = 0xC2548F } });
    NativeSessionFlushTester::receive(*observer.session, proto::CloseWorkspace { .workspace = third });
    NativeSessionFlushTester::receive(*observer.session, proto::ClosePane { .session = f.inFirst.value });
    f.settle();

    CHECK(host.objects().nameOf(ref) == "ops");
    CHECK(host.model().workspace(vtworkspace::WorkspaceId { f.second })->color()
          == std::optional { vtbackend::RGBColor { 0xC2548F } });
    CHECK(host.model().workspace(vtworkspace::WorkspaceId { third }) == nullptr);
    CHECK(host.terminal(inThird) == nullptr);
    CHECK(host.terminal(f.inFirst) == nullptr);
    CHECK(ObserverFixture::refusedBy(observer).empty());
}

TEST_CASE("an observer is no screen mirror", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& observer = f.observe();
    CHECK_FALSE(observer.session->mirrorsScreens());
    CHECK_FALSE(f.flush.h.host.hasScreenMirrors());
    CHECK(observer.session->selectedWorkspace() == 0);

    // A window attaching mirrors; the observer still does not.
    std::ignore = f.attachSelecting(f.first);
    CHECK(f.flush.h.host.hasScreenMirrors());
    CHECK_FALSE(observer.session->mirrorsScreens());
}

TEST_CASE(
    "an observer is not the raise holder while an attach exists, even attached last with nobody focused",
    "[vthost][native][observer][raise]")
{
    auto f = ObserverFixture {};
    auto& window = f.attachSelecting(f.first);
    auto& observer = f.observe();

    CHECK(f.flush.h.host.raiseHolderNow() == std::optional { window.session->connectionId() });
    CHECK(SummaryFixture::rolesOf(observer) == std::vector { proto::RaiseRole::Quiet });

    // Not even focused: the window showing panes keeps the role.
    SummaryFixture::reportFocus(observer, proto::WindowFocus::Focused);
    f.settle();
    CHECK(f.flush.h.host.raiseHolderNow() == std::optional { window.session->connectionId() });
    CHECK(SummaryFixture::rolesOf(observer) == std::vector { proto::RaiseRole::Quiet });
    CHECK(SummaryFixture::rolesOf(window) == std::vector { proto::RaiseRole::Raise });
}

TEST_CASE("with no attach, the most recently focused observer raises", "[vthost][native][observer][raise]")
{
    auto f = ObserverFixture {};
    auto& one = f.observe();
    CHECK(SummaryFixture::rolesOf(one) == std::vector { proto::RaiseRole::Raise }); // alone, it holds it
    auto& two = f.observe();
    CHECK(SummaryFixture::rolesOf(one) == std::vector { proto::RaiseRole::Raise, proto::RaiseRole::Quiet });
    CHECK(SummaryFixture::rolesOf(two) == std::vector { proto::RaiseRole::Raise }); // attached last

    // Whatever panes its report names, as a window's would; read as any PDU it sends is.
    NativeSessionFlushTester::receive(
        *one.session,
        proto::ReportFocus { .focus = proto::WindowFocus::Focused, .shownSessions = { f.inFirst.value } },
        7);
    f.settle();
    CHECK(ObserverFixture::refusedBy(one).empty());
    CHECK(f.flush.h.host.raiseHolderNow() == std::optional { one.session->connectionId() });
    CHECK(SummaryFixture::rolesOf(one).back() == proto::RaiseRole::Raise);
    CHECK(SummaryFixture::rolesOf(two).back() == proto::RaiseRole::Quiet);
    // Its focus makes no pane watched: an observer shows none.
    CHECK_FALSE(f.flush.h.host.isWatched(f.inFirst.value));

    // A window attaching takes the role from them.
    auto& window = f.attachSelecting(f.first);
    CHECK(f.flush.h.host.raiseHolderNow() == std::optional { window.session->connectionId() });
    CHECK(SummaryFixture::rolesOf(one).back() == proto::RaiseRole::Quiet);
}

TEST_CASE("an observer's MarkSeen clears what it saw, and it is told so", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto& observer = f.observe();
    host.terminal(f.inSecond)->writeToScreen("\a");
    f.settle();
    auto const ledger = [&] {
        auto const entries = host.paneAttention();
        auto const entry =
            std::ranges::find(entries, f.inSecond.value, &std::pair<uint64_t, vthost::PaneAttention>::first);
        REQUIRE(entry != entries.end());
        return entry->second;
    };
    REQUIRE(ledger().bell == proto::PaneBell::Rang);
    auto const skip = observer.received.size();

    NativeSessionFlushTester::receive(
        *observer.session,
        proto::MarkSeen { .session = f.inSecond.value, .seen = { .bell = ledger().bellSerial } },
        7);
    f.settle();

    CHECK(ledger().bell == proto::PaneBell::Silent);
    CHECK(ObserverFixture::refusedBy(observer).empty());
    CHECK(std::ranges::any_of(observer.received | std::views::drop(skip),
                              [&](proto::DecodedFrame const& frame) {
                                  auto const* state = std::get_if<proto::PaneAttentionState>(&frame.pdu);
                                  return state != nullptr && state->session == f.inSecond.value
                                         && state->bell == proto::PaneBell::Silent;
                              }));
}

TEST_CASE("an observer is told a pane's program status, and its bell only as the pane's attention",
          "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    // As the application's presence dials one: sharing program status.
    auto& observer = f.observe(ObserverFixture::promotableHello());
    auto const skip = observer.received.size();

    host.terminal(f.inSecond)->writeToScreen("\033]7501;state=blocked:id=agent\033\\\a");
    f.settle();

    auto const told = observer.received | std::views::drop(skip);
    CHECK(std::ranges::any_of(told, [&](proto::DecodedFrame const& frame) {
        auto const* status = std::get_if<proto::ProgramStatus>(&frame.pdu);
        return status != nullptr && status->session == f.inSecond.value && status->records.size() == 1;
    }));
    // The bell is the attaching window's to ring; the observer learns it from the pane's attention alone, as
    // every bell of a pane it does not mirror reaches it, never as a bell of its own.
    CHECK(std::ranges::any_of(told, [&](proto::DecodedFrame const& frame) {
        auto const* state = std::get_if<proto::PaneAttentionState>(&frame.pdu);
        return state != nullptr && state->session == f.inSecond.value && state->bell == proto::PaneBell::Rang;
    }));
    CHECK(std::ranges::none_of(told, [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::SessionBell>(frame.pdu);
    }));
}

TEST_CASE("an observer does not keep an exit-when-idle daemon alive", "[vthost][native][observer][control]")
{
    auto f = ObserverFixture {};
    auto& observer = f.observe();
    CHECK(observer.session->phase() == NativeSession::ConnectionPhase::Observing);
    CHECK(f.daemon.census.count() == 0);
    CHECK(f.daemon.peakClients == 0);
    CHECK(f.flush.h.host.connectedClients() == 0);
    CHECK(f.flush.h.host.connectedObservers() == 1);
}

TEST_CASE("an observer is not sent the restore announcement", "[vthost][native][observer][restore]")
{
    auto snapshot = vthost::WorkspaceSnapshot {};
    snapshot.workspaces.push_back(vthost::WorkspaceLayoutSnapshot {
        .tabs = { vthost::TabSnapshot { .root = vthost::PaneNode { .pane = vthost::PaneSnapshot {} } } } });
    auto h =
        NativeHarness { HarnessOptions { .restore = vthost::WorkspaceRestore { .snapshot = snapshot } } };
    REQUIRE(h.host.sessionCount() == 1);

    auto const got = h.exchangeUntil({ ObserverFixture::observerHello() }, [](auto const& frames) {
        return std::ranges::any_of(frames, [](proto::DecodedFrame const& frame) {
            return std::holds_alternative<proto::LayoutState>(frame.pdu);
        });
    });

    auto const* hello = serverHelloOf(got);
    REQUIRE(hello != nullptr);
    CHECK(hello->restoredWorkspaces == 0);
    CHECK(hello->restoredTabs == 0);
    CHECK(hello->selectedWorkspace == 0);
    // Kept for the first window that attaches.
    CHECK(h.host.takeRestoreAnnouncement([](uint64_t) { return true; }).has_value());
}

TEST_CASE("a promotion that is the first viewer since a restore is told it by its answer",
          "[vthost][native][observer][restore]")
{
    auto snapshot = vthost::WorkspaceSnapshot {};
    snapshot.workspaces.push_back(vthost::WorkspaceLayoutSnapshot {
        .tabs = { vthost::TabSnapshot { .root = vthost::PaneNode { .pane = vthost::PaneSnapshot {} } } } });
    auto h =
        NativeHarness { HarnessOptions { .restore = vthost::WorkspaceRestore { .snapshot = snapshot } } };
    REQUIRE(h.host.sessionCount() == 1);
    auto const restored = h.host.model().workspaceAt(0)->id().value;

    auto const promotedBy = [restored](std::vector<proto::DecodedFrame> const& frames) {
        return std::ranges::find_if(frames, [restored](proto::DecodedFrame const& frame) {
            auto const* end = std::get_if<proto::LayoutBatchEnd>(&frame.pdu);
            return end != nullptr && end->selected == restored;
        });
    };
    auto const got = h.exchangeUntil(
        { ObserverFixture::promotableHello(), proto::SelectWorkspace { .workspace = restored } },
        [&](auto const& frames) { return promotedBy(frames) != frames.end(); });

    auto const answer = promotedBy(got);
    REQUIRE(answer != got.end());
    auto const& end = std::get<proto::LayoutBatchEnd>(answer->pdu);
    CHECK(end.restoredWorkspaces == 1);
    CHECK(end.restoredTabs == 1);
    // Told once: no later window is.
    CHECK_FALSE(h.host.takeRestoreAnnouncement([](uint64_t) { return true; }).has_value());
}

TEST_CASE("a re-asked promotion to workspace 0 with the same token selects what it made",
          "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto const before = f.flush.h.host.model().workspaceCount();
    auto const token = ObserverFixture::tokenOf(1);

    auto const made = f.promoteToNew(token);
    auto const again = f.promoteToNew(token);

    CHECK(made != 0);
    CHECK(again == made);
    CHECK(f.flush.h.host.model().workspaceCount() == before + 1);
}

TEST_CASE("a promotion to workspace 0 with a new token makes another", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto const before = f.flush.h.host.model().workspaceCount();

    auto const made = f.promoteToNew(ObserverFixture::tokenOf(1));
    auto const another = f.promoteToNew(ObserverFixture::tokenOf(2));

    CHECK(made != 0);
    CHECK(another != 0);
    CHECK(another != made);
    CHECK(f.flush.h.host.model().workspaceCount() == before + 2);
}

TEST_CASE("a withdrawn promotion to workspace 0 closes the workspace it made, a demotion keeps it",
          "[vthost][native][observer]")
{
    // The client dropped the answer of a promotion it withdrew: nobody shows what it made, and nobody will.
    for (auto const cause: { proto::ObserveCause::Withdrawn, proto::ObserveCause::Demoted })
    {
        INFO("cause " << static_cast<int>(cause));
        auto f = ObserverFixture {};
        auto& host = f.flush.h.host;
        auto const before = host.model().workspaceCount();
        auto& client = f.observe(ObserverFixture::promotableHello());
        NativeSessionFlushTester::receive(
            *client.session,
            proto::SelectWorkspace { .workspace = 0, .creationToken = ObserverFixture::tokenOf(1) },
            5);
        f.settle();
        auto const ends = ObserverFixture::batchEndsOf(client);
        auto const answer = std::ranges::find(ends, uint64_t { 5 }, &proto::LayoutBatchEnd::causeSerial);
        REQUIRE(answer != ends.end());
        auto const made = answer->selected;
        REQUIRE(made != 0);
        REQUIRE(host.model().workspaceCount() == before + 1);

        NativeSessionFlushTester::receive(*client.session, proto::ObserveOnly { .cause = cause }, 6);
        f.settle();

        CHECK(client.session->phase() == NativeSession::ConnectionPhase::Observing);
        if (cause == proto::ObserveCause::Withdrawn)
        {
            CHECK(host.model().workspace(vtworkspace::WorkspaceId { made }) == nullptr);
            CHECK(host.model().workspaceCount() == before);
        }
        else
            CHECK(host.model().workspace(vtworkspace::WorkspaceId { made }) != nullptr);
    }
}

TEST_CASE("the restore announcement a withdrawn promotion's answer told is told to the next viewer",
          "[vthost][native][observer][restore]")
{
    auto snapshot = vthost::WorkspaceSnapshot {};
    for ([[maybe_unused]] auto const workspace: std::views::iota(0, 2))
        snapshot.workspaces.push_back(vthost::WorkspaceLayoutSnapshot {
            .tabs = {
                vthost::TabSnapshot { .root = vthost::PaneNode { .pane = vthost::PaneSnapshot {} } } } });
    auto f =
        ObserverFixture { ObserverOptions { .restore = vthost::WorkspaceRestore { .snapshot = snapshot } } };
    auto& host = f.flush.h.host;
    auto const restored = host.model().workspaceAt(0)->id().value;
    auto& observer = f.observe(ObserverFixture::promotableHello());
    auto const skip = observer.received.size();
    NativeSessionFlushTester::receive(*observer.session, proto::SelectWorkspace { .workspace = restored }, 5);
    f.settle();
    auto const ends = ObserverFixture::batchEndsOf(observer, skip);
    auto const answer = std::ranges::find(ends, uint64_t { 5 }, &proto::LayoutBatchEnd::causeSerial);
    REQUIRE(answer != ends.end());
    REQUIRE(answer->restoredWorkspaces == 2);

    NativeSessionFlushTester::receive(
        *observer.session, proto::ObserveOnly { .cause = proto::ObserveCause::Withdrawn }, 6);
    f.settle();

    auto& window = f.attachSelecting(f.first);
    auto const* hello = serverHelloOf(window.received);
    REQUIRE(hello != nullptr);
    CHECK(hello->restoredWorkspaces == 2);
    CHECK(hello->restoredTabs == 2);
}

TEST_CASE("a remembered token whose workspace closed makes a new one", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto const token = ObserverFixture::tokenOf(1);
    auto const made = f.promoteToNew(token);
    REQUIRE(made != 0);
    REQUIRE(host.closeWorkspace(vtworkspace::WorkspaceId { made }).has_value());
    f.settle();
    auto const before = host.model().workspaceCount();

    auto const again = f.promoteToNew(token);

    CHECK(again != 0);
    CHECK(again != made);
    CHECK(host.model().workspaceCount() == before + 1);
}

TEST_CASE("tokens beyond the memory forget the oldest", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto const oldest = ObserverFixture::tokenOf(0);
    auto const made = f.promoteToNew(oldest);
    REQUIRE(made != 0);
    // As many later promotions as the host remembers.
    for (auto const seed:
         std::views::iota(uint8_t { 1 }, static_cast<uint8_t>(vthost::PromotionTokenMemory + 1)))
        host.rememberWorkspaceMadeFor(
            { .token = ObserverFixture::tokenOf(seed), .workspace = vtworkspace::WorkspaceId { made } });
    REQUIRE(host.workspaceMadeFor(ObserverFixture::tokenOf(1)).transform([](auto const& entry) {
        return entry.workspace;
    }) == vtworkspace::WorkspaceId { made });
    auto const before = host.model().workspaceCount();

    auto const again = f.promoteToNew(oldest);

    CHECK(again != made);
    CHECK(host.model().workspaceCount() == before + 1);
}

TEST_CASE("the restore announcement a dropped promotion took is told to the next viewer",
          "[vthost][native][observer][restore]")
{
    auto snapshot = vthost::WorkspaceSnapshot {};
    for ([[maybe_unused]] auto const workspace: std::views::iota(0, 2))
        snapshot.workspaces.push_back(vthost::WorkspaceLayoutSnapshot {
            .tabs = {
                vthost::TabSnapshot { .root = vthost::PaneNode { .pane = vthost::PaneSnapshot {} } } } });
    auto f =
        ObserverFixture { ObserverOptions { .restore = vthost::WorkspaceRestore { .snapshot = snapshot } } };
    auto& host = f.flush.h.host;
    auto const restored = host.model().workspaceAt(0)->id().value;
    auto& observer = f.observe(ObserverFixture::promotableHello());

    auto const skip = observer.received.size();

    // Promoted, its answer spawned but not yet run: the connection drops first.
    NativeSessionFlushTester::receive(*observer.session, proto::SelectWorkspace { .workspace = restored }, 5);
    REQUIRE(observer.session->phase() == NativeSession::ConnectionPhase::Viewing);
    REQUIRE(NativeSessionFlushTester::letGo(*observer.session, f.flush.h.loop, f.flush.clock));
    observer.session.reset();
    f.settle();
    REQUIRE(ObserverFixture::batchEndsOf(observer, skip).empty());

    auto& window = f.attachSelecting(f.first);

    auto const* hello = serverHelloOf(window.received);
    REQUIRE(hello != nullptr);
    CHECK(hello->restoredWorkspaces == 2);
    CHECK(hello->restoredTabs == 2);
}

TEST_CASE("an observer's promotion to workspace 0 starts the shell where the pane that asked is",
          "[vthost][native][observer][cwd]")
{
    auto const spawned = std::make_shared<std::vector<std::filesystem::path>>();
    auto h = NativeHarness { recordingSpawnDirectories(spawned) };
    h.files.addDirectory(statedDirectory());
    auto const* const tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const from = tab->activePane()->session();
    auto* const terminal = h.host.terminal(from);
    REQUIRE(terminal != nullptr);
    terminal->writeToScreen(std::format("\033]7;{}\033\\", vtbackend::test::fileUrlOf(statedDirectory())));
    spawned->clear();

    std::ignore =
        h.exchangeUntil({ ObserverFixture::promotableHello(),
                          proto::SelectWorkspace { .workspace = 0, .directoryFromSession = from.value } },
                        [](auto const& got) { return layoutWorkspaces(got).size() == 2; });

    CHECK(*spawned == std::vector { statedDirectory() });
}

TEST_CASE("an observer is sent every workspace's layout and summaries, none mirrored",
          "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& observer = f.observe();

    auto const layoutFor = [&](uint64_t workspace) {
        return std::ranges::any_of(observer.received, [workspace](proto::DecodedFrame const& frame) {
            auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu);
            return layout != nullptr && layout->workspace == workspace;
        });
    };
    CHECK(layoutFor(f.first));
    CHECK(layoutFor(f.second));
    CHECK(MetaFixture::metasOf(observer).size() == 2);
    CHECK(SummaryFixture::summariesOf(observer, f.inFirst).size() == 1);
    CHECK(SummaryFixture::summariesOf(observer, f.inSecond).size() == 1);
    CHECK(snapshotSessions(observer.received).empty());

    // What a pane raises reaches it as a summary and an attention state, never as the pane's own event.
    auto const skip = observer.received.size();
    f.flush.h.host.terminal(f.inFirst)->writeToScreen("\a");
    f.settle();
    auto const summaries = SummaryFixture::summariesOf(observer, f.inFirst, skip);
    CHECK(std::ranges::count(summaries, proto::PaneBell::Rang, &proto::PaneSummary::bell) == 1);
    CHECK(SummaryFixture::countOf<proto::SessionBell>(observer, f.inFirst) == 0);
    CHECK(std::ranges::any_of(observer.received | std::views::drop(skip),
                              [&](proto::DecodedFrame const& frame) {
                                  auto const* state = std::get_if<proto::PaneAttentionState>(&frame.pdu);
                                  return state != nullptr && state->session == f.inFirst.value
                                         && state->bell == proto::PaneBell::Rang;
                              }));
    CHECK(snapshotSessions(observer.received).empty());
}

TEST_CASE("an observer is never a clipboard target", "[vthost][native][observer][clipboard]")
{
    auto f = ObserverFixture {};
    auto& observer = f.observe();
    SummaryFixture::reportFocus(observer, proto::WindowFocus::Focused);

    f.flush.h.host.terminal(f.inFirst)->writeToScreen("\033]52;c;aGk=\033\\");
    f.settle();
    CHECK(SummaryFixture::countOf<proto::SessionClipboard>(observer, f.inFirst) == 0);

    // Beside a window showing the pane, the window lands it.
    auto& window = f.attachSelecting(f.first);
    SummaryFixture::reportFocus(observer, proto::WindowFocus::Focused);
    f.flush.h.host.terminal(f.inFirst)->writeToScreen("\033]52;c;aGk=\033\\");
    f.settle();
    CHECK(SummaryFixture::countOf<proto::SessionClipboard>(observer, f.inFirst) == 0);
    CHECK(SummaryFixture::countOf<proto::SessionClipboard>(window, f.inFirst) == 1);
}

TEST_CASE("an observer fires no client.attached or client.detached", "[vthost][native][observer][control]")
{
    auto f = ObserverFixture {};
    auto& observer = f.observe();
    CHECK(f.control.attached.empty());
    observer.session.reset();
    CHECK(f.control.detached.empty());
    CHECK(f.flush.h.host.connectedObservers() == 0);
}
TEST_CASE("a promoted observer equals Attach+Select", "[vthost][native][observer]")
{
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto f = ObserverFixture { ObserverOptions { .extensions = hosting } };
    auto& host = f.flush.h.host;
    // A question asked while no window shows the pane, and a notice kept for the first window.
    auto const asked = proto::PermissionRequest { .session = 0, .hostId = "git@/x/git.wasm", .name = "git" };
    hosting->sessions.front()->askPermission(asked);
    REQUIRE(turnUntil(f.flush.h.loop, [&] { return !host.pendingPermissionRequests().empty(); }));
    host.announceDegradation("the configuration no longer loads");

    auto hello = ObserverFixture::promotableHello();
    hello.clientArea = proto::WireClientArea { .columns = 120, .lines = 40 };
    auto& observer = f.observe(std::move(hello));
    auto const holds = [&](auto isWanted, std::size_t skip) {
        return std::ranges::any_of(observer.received | std::views::drop(skip), isWanted);
    };
    auto const isQuestion = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::PermissionRequest>(frame.pdu);
    };
    auto const isNotice = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::SessionNotify>(frame.pdu);
    };
    REQUIRE_FALSE(holds(isQuestion, 0));
    REQUIRE_FALSE(holds(isNotice, 0));
    REQUIRE(f.daemon.census.count() == 0);
    auto const skip = observer.received.size();

    f.select(observer, f.first);

    auto const id = observer.session->connectionId();
    CHECK(observer.session->phase() == NativeSession::ConnectionPhase::Viewing);
    CHECK(observer.session->selectedWorkspace() == f.first);
    CHECK(observer.session->mirrorsScreens());
    // Counted as an attach is: by the host, the census and the control API.
    CHECK(host.connectedClients() == 1);
    CHECK(host.connectedObservers() == 0);
    CHECK(f.daemon.census.count() == 1);
    CHECK(f.control.attached == std::vector { std::format("{}", id) });
    // The only window: it raises, as it did as the only observer, and was told so once.
    CHECK(host.raiseHolderNow() == std::optional { id });
    CHECK(SummaryFixture::rolesOf(observer) == std::vector { proto::RaiseRole::Raise });
    // Its held area is the daemon's.
    auto const area = vtpty::PageSize { vtpty::LineCount(40), vtpty::ColumnCount(120) };
    CHECK(host.terminal(f.inFirst)->pageSize() == area);
    // The selected workspace's screens in full, the question and the notice waiting for a window.
    CHECK(snapshotSessions(observer.received) == std::set { f.inFirst.value });
    CHECK(holds(isQuestion, skip));
    CHECK(holds(isNotice, skip));
    CHECK(host.pendingPermissionRequests().size() == 1); // asked, not answered
    // Answered by the batch, stating the selection.
    CHECK(ObserverFixture::batchEndsOf(observer, skip).back()
          == proto::LayoutBatchEnd { .causeSerial = 5, .selected = f.first });
    CHECK(ObserverFixture::refusedBy(observer, skip).empty());
}

TEST_CASE("a promoted observer is handed the host-drawn indicator rows current", "[vthost][native][observer]")
{
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    hosting->prepare = [](vtbackend::FakeStatusLineExtensions& fake) {
        fake.takeOver("plasma");
        fake.publish("plasma", vthost::testing::frameOf("FIRST"));
    };
    auto f = ObserverFixture { ObserverOptions { .extensions = hosting } };
    auto* terminal = f.flush.h.host.terminal(f.inFirst);
    REQUIRE(terminal != nullptr);
    auto const row = [&] {
        auto const _ = std::lock_guard { *terminal };
        return terminal->indicatorStatusLineDisplay().grid().lineAt(vtbackend::LineOffset(0)).toUtf8Trimmed();
    };
    auto& observer = f.observe(ObserverFixture::promotableHello());
    // Shown to no one: a new frame repaints nothing.
    for (auto* extensions: hosting->sessions)
        extensions->publish("plasma", vthost::testing::frameOf("SECOND", 2));
    f.settle();
    REQUIRE(row() != "SECOND");

    f.select(observer, f.first);

    CHECK(row() == "SECOND");
}

TEST_CASE("promotion to workspace 0 creates one and selects it", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto& observer = f.observe(ObserverFixture::promotableHello());
    auto const before = host.model().workspaceCount();

    f.select(observer, 0);

    CHECK(host.model().workspaceCount() == before + 1);
    auto const selected = observer.session->selectedWorkspace();
    CHECK(selected != f.first);
    CHECK(selected != f.second);
    CHECK(observer.session->phase() == NativeSession::ConnectionPhase::Viewing);
    CHECK(ObserverFixture::batchEndsOf(observer).back().selected == selected);
}

TEST_CASE("promotion to workspace 0 spawns its shell at the held area", "[vthost][native][observer]")
{
    auto f = ObserverFixture { ObserverOptions { .ptyFactory = countingPtys().ptyFactory } };
    auto& host = f.flush.h.host;
    auto& observer = f.observe(ObserverFixture::promotableHello());
    NativeSessionFlushTester::receive(*observer.session,
                                      proto::ResizeRequest { .columns = 132, .lines = 43 });
    f.settle();

    f.select(observer, 0);

    auto const selected = observer.session->selectedWorkspace();
    REQUIRE(selected != 0);
    auto const sessions = sessionsOf(host, selected);
    REQUIRE(sessions.size() == 1);
    auto const made = vtworkspace::SessionId { *sessions.begin() };
    CHECK(host.terminal(made)->pageSize()
          == vtpty::PageSize { vtpty::LineCount(43), vtpty::ColumnCount(132) });
    // Spawned at it, not at the host's size and corrected after: its one resize is the spawn's own.
    CHECK(countingPtyOf(f.flush.h, made).resizes() == 1);
}

TEST_CASE("a promoted observer's hello colour preference is not applied", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto const schemeBefore = host.defaultColorPreference();
    auto hello = ObserverFixture::promotableHello();
    hello.colorPreference = schemeBefore == vtbackend::ColorPreference::Dark
                                ? proto::WireColorPreference::Light
                                : proto::WireColorPreference::Dark;
    auto& observer = f.observe(std::move(hello));

    f.select(observer, f.first);

    // Stated when it attached, it may be stale by now: the promoted window states its own on each pane.
    REQUIRE(observer.session->phase() == NativeSession::ConnectionPhase::Viewing);
    CHECK(host.defaultColorPreference() == schemeBefore);
}

TEST_CASE("promotion to an unknown workspace is refused and leaves an observer", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto& observer = f.observe(ObserverFixture::promotableHello());
    auto const skip = observer.received.size();

    f.select(observer, 999);

    CHECK(observer.session->phase() == NativeSession::ConnectionPhase::Observing);
    CHECK(observer.session->selectedWorkspace() == 0);
    CHECK(host.connectedObservers() == 1);
    CHECK(host.connectedClients() == 0);
    CHECK(f.daemon.census.count() == 0);
    CHECK(f.control.attached.empty());
    CHECK(snapshotSessions(observer.received).empty());
    // Said by the batch, which states no selection; not as a request the daemon does not know.
    CHECK(ObserverFixture::batchEndsOf(observer, skip)
          == std::vector { proto::LayoutBatchEnd { .causeSerial = 5, .selected = 0 } });
    CHECK(ObserverFixture::refusedBy(observer, skip).empty());
}

TEST_CASE("promotion with a held area re-projects panes once", "[vthost][native][observer]")
{
    auto f = ObserverFixture { ObserverOptions { .ptyFactory = countingPtys().ptyFactory } };
    auto& host = f.flush.h.host;
    auto& observer = f.observe(ObserverFixture::promotableHello());
    NativeSessionFlushTester::receive(
        *observer.session,
        proto::ResizeRequest { .columns = 132,
                               .lines = 43,
                               .cellPixelSize = proto::WireCellPixelSize { .width = 9, .height = 18 } });
    f.settle();
    auto const resizesBefore = countingPtyOf(f.flush.h, f.inFirst).resizes();
    auto const otherBefore = countingPtyOf(f.flush.h, f.inSecond).resizes();

    f.select(observer, f.first);

    auto const area = vtpty::PageSize { vtpty::LineCount(43), vtpty::ColumnCount(132) };
    CHECK(host.terminal(f.inFirst)->pageSize() == area);
    CHECK(host.terminal(f.inFirst)->cellPixelSize() == cellOf(9, 18));
    CHECK(countingPtyOf(f.flush.h, f.inFirst).resizes() == resizesBefore + 1);
    CHECK(countingPtyOf(f.flush.h, f.inSecond).resizes() == otherBefore + 1);
    // Its one snapshot of the pane is the attach's: the resize is not answered by another.
    CHECK(std::ranges::count_if(observer.received,
                                [&](proto::DecodedFrame const& frame) {
                                    auto const* state = std::get_if<proto::SessionState>(&frame.pdu);
                                    return state != nullptr && state->session == f.inFirst.value;
                                })
          == 1);
}

TEST_CASE("demotion tells Quiet and passes the raise role on", "[vthost][native][observer][raise]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto& window = f.attachDemotable(f.first);
    auto& leaving = f.attachDemotable(f.second);
    SummaryFixture::reportFocus(leaving, proto::WindowFocus::Focused);
    f.settle();
    REQUIRE(host.raiseHolderNow() == std::optional { leaving.session->connectionId() });
    REQUIRE(SummaryFixture::rolesOf(leaving).back() == proto::RaiseRole::Raise);

    f.demote(leaving);

    CHECK(leaving.session->phase() == NativeSession::ConnectionPhase::Observing);
    CHECK(SummaryFixture::rolesOf(leaving).back() == proto::RaiseRole::Quiet);
    CHECK(host.raiseHolderNow() == std::optional { window.session->connectionId() });
    CHECK(SummaryFixture::rolesOf(window).back() == proto::RaiseRole::Raise);
    CHECK(host.connectedClients() == 1);
    CHECK(host.connectedObservers() == 1);
    CHECK(f.daemon.census.count() == 1);

    SECTION("an observer left alone raises again, once told Quiet")
    {
        window.session.reset();
        f.settle();
        CHECK(host.raiseHolderNow() == std::optional { leaving.session->connectionId() });
        auto const roles = SummaryFixture::rolesOf(leaving);
        CHECK(std::vector(roles.end() - 2, roles.end())
              == std::vector { proto::RaiseRole::Quiet, proto::RaiseRole::Raise });
    }
}

TEST_CASE("demotion of the only window tells it Quiet before it raises as an observer",
          "[vthost][native][observer][raise]")
{
    auto f = ObserverFixture {};
    auto& alone = f.attachDemotable(f.first);
    REQUIRE(SummaryFixture::rolesOf(alone).back() == proto::RaiseRole::Raise);
    auto const before = SummaryFixture::rolesOf(alone).size();

    f.demote(alone);

    auto const roles = SummaryFixture::rolesOf(alone);
    CHECK(std::vector(roles.begin() + static_cast<std::ptrdiff_t>(before), roles.end())
          == std::vector { proto::RaiseRole::Quiet, proto::RaiseRole::Raise });
    CHECK(f.flush.h.host.raiseHolderNow() == std::optional { alone.session->connectionId() });
}

TEST_CASE("demotion re-resolves the client area without it", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    auto& staying = f.attachDemotable(f.first);
    auto& leaving = f.attachDemotable(f.second);
    NativeSessionFlushTester::receive(*staying.session, proto::ResizeRequest { .columns = 100, .lines = 30 });
    f.settle();
    NativeSessionFlushTester::receive(*leaving.session, proto::ResizeRequest { .columns = 120, .lines = 40 });
    f.settle();
    auto const large = vtpty::PageSize { vtpty::LineCount(40), vtpty::ColumnCount(120) };
    REQUIRE(host.terminal(f.inFirst)->pageSize() == large);

    f.demote(leaving);

    auto const remaining = vtpty::PageSize { vtpty::LineCount(30), vtpty::ColumnCount(100) };
    CHECK(host.terminal(f.inFirst)->pageSize() == remaining);
    CHECK(host.terminal(f.inSecond)->pageSize() == remaining);
    // What it reports from now on is held, as any observer's is.
    NativeSessionFlushTester::receive(*leaving.session, proto::ResizeRequest { .columns = 150, .lines = 50 });
    f.settle();
    CHECK(host.terminal(f.inFirst)->pageSize() == remaining);
}

TEST_CASE("after demotion its workspace is free for the next Default attach", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& host = f.flush.h.host;
    std::ignore = f.attachDemotable(f.first);
    auto& leaving = f.attachDemotable(f.second);
    auto const workspaces = host.model().workspaceCount();

    f.demote(leaving);

    CHECK(leaving.session->selectedWorkspace() == 0);
    CHECK(host.visibility(f.second) == vthost::WorkspaceVisibility::Hidden);
    auto& next = f.flush.connect();
    NativeSessionFlushTester::handshake(
        *next.session,
        proto::ClientHello { .capabilities = proto::CapabilitySet::of(
                                 { proto::Capability::AttachIntent, proto::Capability::DaemonFollow }),
                             .attachIntent = { .kind = proto::AttachIntentKind::Default } });
    f.settle();
    CHECK(next.session->selectedWorkspace() == f.second);
    CHECK(host.model().workspaceCount() == workspaces);
}

TEST_CASE("a demoted connection mirrors nothing and is summarized every pane", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& leaving = f.attachDemotable(f.first);
    REQUIRE(NativeSessionFlushTester::mirrors(*leaving.session, f.inFirst));
    AttentionFixture::reportShown(leaving, proto::WindowFocus::Focused, { f.inFirst });
    auto const skip = leaving.received.size();

    f.demote(leaving);

    CHECK_FALSE(NativeSessionFlushTester::mirrors(*leaving.session, f.inFirst));
    CHECK_FALSE(leaving.session->mirrorsScreens());
    CHECK_FALSE(f.flush.h.host.hasScreenMirrors());
    // Nothing stays watched on its account.
    CHECK_FALSE(f.flush.h.host.isWatched(f.inFirst.value));
    CHECK_FALSE(SummaryFixture::summariesOf(leaving, f.inFirst, skip).empty());
    CHECK_FALSE(SummaryFixture::summariesOf(leaving, f.inSecond, skip).empty());

    // Output of the pane it showed reaches it as a summary, no longer as its screen.
    auto const after = leaving.received.size();
    f.flush.h.host.terminal(f.inFirst)->writeToScreen("x\a");
    leaving.session->sessionScreenUpdated(f.inFirst);
    f.settle();
    CHECK(incrementCount(std::vector(leaving.received.begin() + static_cast<std::ptrdiff_t>(after),
                                     leaving.received.end()))
          == 0);
    CHECK(SummaryFixture::countOf<proto::SessionBell>(leaving, f.inFirst) == 0);
}

TEST_CASE("demotion is answered by a batch selecting nothing", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& leaving = f.attachDemotable(f.first);
    auto const skip = leaving.received.size();

    f.demote(leaving);

    // Named by the demotion's serial: what came before it the client tells as older.
    CHECK(ObserverFixture::batchEndsOf(leaving, skip)
          == std::vector { proto::LayoutBatchEnd { .causeSerial = 6, .selected = 0 } });
}

TEST_CASE("ObserveOnly from an observer is a no-op", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& observer = f.observe();
    auto const skip = observer.received.size();

    f.demote(observer);

    CHECK(observer.session->phase() == NativeSession::ConnectionPhase::Observing);
    CHECK(f.flush.h.host.connectedObservers() == 1);
    CHECK(observer.received.size() == skip);
}

TEST_CASE("ObserveOnly from a window that cannot observe is refused", "[vthost][native][observer]")
{
    auto f = ObserverFixture {};
    auto& window = f.attachSelecting(f.first); // shares no Observe

    f.demote(window);

    CHECK(window.session->phase() == NativeSession::ConnectionPhase::Viewing);
    CHECK(ObserverFixture::refusedBy(window)
          == std::vector<uint64_t> { std::to_underlying(proto::PduType::ObserveOnly) });
}

TEST_CASE("demotion fires client.detached", "[vthost][native][observer][control]")
{
    auto f = ObserverFixture {};
    auto& leaving = f.attachDemotable(f.first);
    auto const id = std::format("{}", leaving.session->connectionId());
    REQUIRE(f.control.attached == std::vector { id });
    REQUIRE(f.daemon.census.count() == 1);

    f.demote(leaving);

    CHECK(f.control.detached == std::vector { id });
    CHECK(f.daemon.census.count() == 0);
    // Once: the connection ending as an observer says nothing more.
    leaving.session.reset();
    CHECK(f.control.detached == std::vector { id });
}
// }}}
