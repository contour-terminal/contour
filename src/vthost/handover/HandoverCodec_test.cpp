// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/StateFields.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <ranges>
#include <span>
#include <vector>

#include <vthost/SessionSettings.hpp>
#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/testing/RecordedNameEncodings.hpp>

using namespace vthost;
using namespace vthost::handover;

namespace
{

[[nodiscard]] vtcontrol::Uuid uuidOf(uint8_t seed)
{
    auto uuid = vtcontrol::Uuid {};
    uuid.bytes.fill(seed);
    return uuid;
}

[[nodiscard]] vtworkspace::PaneTreeState leaf(uint64_t pane, uint64_t session)
{
    return vtworkspace::PaneTreeState { .id = vtworkspace::PaneId { pane },
                                        .split = vtworkspace::SplitState::None,
                                        .ratio = 0.5,
                                        .session = vtworkspace::SessionId { session },
                                        .children = {} };
}

[[nodiscard]] HandoverSession sessionOf(uint64_t id)
{
    auto session = HandoverSession {};
    session.id = vtworkspace::SessionId { id };
    session.uuid = uuidOf(static_cast<uint8_t>(id));
    session.name = "shell";
    session.profileName = "default";
    session.origin = SettingsOrigin::Client;
    auto settings = defaultSessionSettings();
    settings.maxImageRegisterCount = 1024;
    settings.wordDelimiters = U" /\\()";
    session.statedSettings = toWireSessionSettings(settings);
    session.pty.masterFd = 11;
    session.pty.stdoutFastPipe = std::pair { 12, -1 };
    session.pty.pageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };
    session.pty.pixels = vtpty::ImageSize { vtpty::Width(800), vtpty::Height(600) };
    session.child.pid = 4242;
    session.child.executable = "/bin/sh";
    session.child.args = { "sh", "-l" };
    session.child.cwd = "/home/user";
    session.child.hangupGrace.duration = std::chrono::milliseconds { 350 };
    session.terminalState = { 1, 2, 3, 4, 5 };
    session.carried = { CarriedDelivery { .kind = DeliveryKind::Pty,
                                          .bytes = "ls\r",
                                          .progress = DeliveryProgress::InterruptedRest },
                        CarriedDelivery { .kind = DeliveryKind::Screen, .bytes = "\033[2J" } };
    if (id % 2 == 0)
    {
        session.colorPreference = vtbackend::ColorPreference::Dark;
        session.interruption =
            CommandInterruption { .commandLine = "make -j8", .after = vtbackend::CommandBlockId(41) };
        session.device = SessionDevice::PtyOnly;
    }
    return session;
}

[[nodiscard]] HandoverImage sampleImage()
{
    auto image = HandoverImage {};
    image.daemonInstance = 0xDEADBEEFCAFEULL;
    image.fromRelease = "0.7.1";
    auto split = vtworkspace::PaneTreeState { .id = vtworkspace::PaneId { 3 },
                                              .split = vtworkspace::SplitState::Vertical,
                                              .ratio = 0.25,
                                              .session = {},
                                              .children = { leaf(1, 1), leaf(2, 2) } };
    auto tab = vtworkspace::TabState {};
    tab.id = vtworkspace::TabId { 5 };
    tab.root = split;
    tab.view.activePane = vtworkspace::PaneId { 2 };
    tab.view.zoom = vtworkspace::ZoomState::Zoomed;
    tab.view.recentPanes = { vtworkspace::PaneId { 2 }, vtworkspace::PaneId { 1 } };
    tab.view.title = "build";
    tab.view.colors[1] = vtbackend::RGBColor { 1, 2, 3 };
    auto workspace = vtworkspace::WorkspaceState {};
    workspace.id = vtworkspace::WorkspaceId { 9 };
    workspace.tabs = { tab };
    workspace.activeTabIndex = 0;
    image.tree.workspaces = { workspace };
    image.modelIds = { vtworkspace::WorkspaceId { 10 }, vtworkspace::TabId { 6 }, vtworkspace::PaneId { 4 } };
    image.nextSessionId = vtworkspace::SessionId { 3 };
    image.objects.workspaces = { BoundObject { .id = 9, .uuid = uuidOf(0xA1) } };
    image.objects.tabs = { BoundObject { .id = 5, .uuid = uuidOf(0xA2) } };
    image.objects.panes = { BoundObject { .id = 1, .uuid = uuidOf(0xA3) },
                            BoundObject { .id = 2, .uuid = uuidOf(0xA4) } };
    image.objects.names = { NamedObject { .ref = { vtcontrol::ObjectKind::Tab, uuidOf(0xA2) },
                                          .name = "work" } };
    image.sessions = { sessionOf(1), sessionOf(2) };
    image.listeners = { InheritedListener { .name = "control", .handle = vtpty::CrossingHandle { 20 } },
                        InheritedListener { .name = "native-tcp",
                                            .handle = vtpty::CrossingHandle { 21 },
                                            .tlsIdentity =
                                                TlsIdentity { .certPem = "-----BEGIN CERTIFICATE-----",
                                                              .keyPem = "-----BEGIN PRIVATE KEY-----" } } };
    image.lease = vtpty::CrossingHandle { 30 };
    image.defaultColorPreference = vtbackend::ColorPreference::Dark;
    image.upgradeGeneration = 4;
    image.cellPixelSize = vtpty::ImageSize { vtpty::Width(9), vtpty::Height(18) };
    return image;
}

/// Compares what the codec promises to carry.
void requireSame(HandoverImage const& actual, HandoverImage const& expected)
{
    CHECK(actual.daemonInstance == expected.daemonInstance);
    CHECK(actual.fromRelease == expected.fromRelease);
    CHECK(actual.nextSessionId == expected.nextSessionId);
    CHECK(actual.lease == expected.lease);
    CHECK(actual.upgradeGeneration == expected.upgradeGeneration);
    CHECK(actual.modelIds.nextWorkspace == expected.modelIds.nextWorkspace);
    CHECK(actual.modelIds.nextTab == expected.modelIds.nextTab);
    CHECK(actual.modelIds.nextPane == expected.modelIds.nextPane);
    REQUIRE(actual.tree.workspaces.size() == expected.tree.workspaces.size());
    auto const& workspace = actual.tree.workspaces.at(0);
    auto const& wanted = expected.tree.workspaces.at(0);
    CHECK(workspace.id == wanted.id);
    CHECK(workspace.activeTabIndex == wanted.activeTabIndex);
    CHECK(workspace.previousActiveTabIndex == wanted.previousActiveTabIndex);
    REQUIRE(workspace.tabs.size() == 1);
    auto const& tab = workspace.tabs.at(0);
    CHECK(tab.id == wanted.tabs.at(0).id);
    CHECK(tab.root.split == vtworkspace::SplitState::Vertical);
    CHECK(tab.root.ratio == 0.25);
    REQUIRE(tab.root.children.size() == 2);
    CHECK(tab.root.children.at(1).session == vtworkspace::SessionId { 2 });
    CHECK(tab.view.zoom == vtworkspace::ZoomState::Zoomed);
    CHECK(tab.view.recentPanes == wanted.tabs.at(0).view.recentPanes);
    CHECK(tab.view.title == std::optional<std::string> { "build" });
    CHECK(tab.view.colors == wanted.tabs.at(0).view.colors);
    CHECK(actual.objects.workspaces.size() == 1);
    CHECK(actual.objects.panes.at(1).uuid == uuidOf(0xA4));
    CHECK(actual.objects.names.at(0).name == "work");
    CHECK(actual.objects.names.at(0).ref == expected.objects.names.at(0).ref);
    CHECK(actual.defaultColorPreference == expected.defaultColorPreference);
    CHECK(actual.cellPixelSize == expected.cellPixelSize);
    CHECK(actual.listeners.size() == 2);
    CHECK(actual.listeners.at(1).name == "native-tcp");
    CHECK(actual.listeners.at(1).handle == vtpty::CrossingHandle { 21 });
    CHECK_FALSE(actual.listeners.at(0).tlsIdentity.has_value());
    REQUIRE(actual.listeners.at(1).tlsIdentity.has_value());
    CHECK(actual.listeners.at(1).tlsIdentity->certPem == expected.listeners.at(1).tlsIdentity->certPem);
    CHECK(actual.listeners.at(1).tlsIdentity->keyPem == expected.listeners.at(1).tlsIdentity->keyPem);
    REQUIRE(actual.sessions.size() == expected.sessions.size());
    for (auto const index: std::views::iota(size_t { 0 }, expected.sessions.size()))
    {
        auto const& got = actual.sessions.at(index);
        auto const& want = expected.sessions.at(index);
        CHECK(got.id == want.id);
        CHECK(got.uuid == want.uuid);
        CHECK(got.name == want.name);
        CHECK(got.profileName == want.profileName);
        CHECK(got.origin == want.origin);
        CHECK(got.statedSettings == want.statedSettings);
        CHECK(got.pty.masterFd == want.pty.masterFd);
        CHECK(got.pty.stdoutFastPipe == want.pty.stdoutFastPipe);
        CHECK(got.pty.pageSize.lines == want.pty.pageSize.lines);
        CHECK(got.pty.pageSize.columns == want.pty.pageSize.columns);
        CHECK(got.pty.pixels.has_value());
        CHECK(got.child.pid == want.child.pid);
        CHECK(got.child.executable == want.child.executable);
        CHECK(got.child.args == want.child.args);
        CHECK(got.child.cwd == want.child.cwd);
        CHECK(got.child.hangupGrace.duration == want.child.hangupGrace.duration);
        CHECK(got.terminalState == want.terminalState);
        CHECK(got.carried == want.carried);
        CHECK(got.colorPreference == want.colorPreference);
        CHECK(got.interruption == want.interruption);
    }
}

/// @return @p payload, framed under the handover header.
[[nodiscard]] std::vector<uint8_t> framed(std::vector<uint8_t> const& payload, uint32_t version)
{
    auto writer = vtbackend::StateWriter::payload();
    writer.raw(HandoverMagic);
    writer.varint(version);
    writer.bytes(payload);
    return std::move(writer).take();
}

/// A fixed-sequence generator (SplitMix64): the fuzz below is the same on every run and every platform.
class SplitMix64
{
  public:
    explicit SplitMix64(uint64_t state) noexcept: _state { state } {}

    [[nodiscard]] uint64_t next() noexcept
    {
        _state += 0x9E3779B97F4A7C15ULL;
        auto z = _state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

  private:
    uint64_t _state;
};

} // namespace

TEST_CASE("a handover image round-trips", "[handover]")
{
    auto const image = sampleImage();
    auto const decoded = decodeHandover(encodeHandover(image));
    REQUIRE(decoded.has_value());
    requireSame(*decoded, image);
}

TEST_CASE("a session whose PTY has an empty page size is decoded without its state, at a usable size",
          "[handover]")
{
    auto image = sampleImage();
    auto const empty = GENERATE(vtpty::PageSize { vtpty::LineCount(0), vtpty::ColumnCount(0) },
                                vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(0) },
                                vtpty::PageSize { vtpty::LineCount(0), vtpty::ColumnCount(80) });
    image.sessions.front().pty.pageSize = empty;

    // The image is not refused: the program is still adopted, on a fresh terminal.
    auto const decoded = decodeHandover(encodeHandover(image));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->sessions.size() == 2);
    auto const& refused = decoded->sessions.front();
    CHECK(refused.pty.pageSize.lines == FallbackPageSize.lines);
    CHECK(refused.pty.pageSize.columns == FallbackPageSize.columns);
    CHECK(refused.terminalState.empty());
    CHECK(refused.pty.masterFd == image.sessions.front().pty.masterFd);
    // The other session keeps everything.
    CHECK(decoded->sessions.back().terminalState == image.sessions.back().terminalState);
}

TEST_CASE("a listener and the lease cross as one handle each", "[handover]")
{
    auto image = HandoverImage {};
    image.lease = vtpty::CrossingHandle { 7 };
    image.listeners = { InheritedListener { .name = "native", .handle = vtpty::CrossingHandle { 9 } } };
    auto const decoded = decodeHandover(encodeHandover(image));
    REQUIRE(decoded.has_value());
    CHECK(decoded->lease == vtpty::CrossingHandle { 7 });
    REQUIRE(decoded->listeners.size() == 1);
    CHECK(decoded->listeners.at(0).name == "native");
    CHECK(decoded->listeners.at(0).handle == vtpty::CrossingHandle { 9 });
}

TEST_CASE("a none handle round-trips as none", "[handover]")
{
    auto image = HandoverImage {};
    image.lease = vtpty::CrossingHandle {};
    image.listeners = { InheritedListener { .name = "control", .handle = vtpty::CrossingHandle {} },
                        InheritedListener { .name = "native", .handle = vtpty::CrossingHandle { 0 } } };
    auto const decoded = decodeHandover(encodeHandover(image));
    REQUIRE(decoded.has_value());
    CHECK(decoded->lease.isNone());
    REQUIRE(decoded->listeners.size() == 2);
    CHECK(decoded->listeners.at(0).handle.isNone());
    // Zero is a handle, not none.
    CHECK(decoded->listeners.at(1).handle == vtpty::CrossingHandle { 0 });
}

TEST_CASE("equal images encode to equal bytes", "[handover]")
{
    CHECK(encodeHandover(sampleImage()) == encodeHandover(sampleImage()));
}

TEST_CASE("format above HandoverFormatVersion is UnknownFormatVersion", "[handover]")
{
    auto stream = encodeHandover(sampleImage());
    REQUIRE(stream.at(HandoverMagic.size()) == HandoverFormatVersion);
    stream.at(HandoverMagic.size()) = static_cast<uint8_t>(HandoverFormatVersion + 1);
    auto const decoded = decodeHandover(stream);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == HandoverError::UnknownFormatVersion);
}

TEST_CASE("a stream that is not a handover image is Malformed", "[handover]")
{
    auto stream = encodeHandover(sampleImage());
    stream.at(0) = 'X';
    auto const decoded = decodeHandover(stream);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == HandoverError::Malformed);
}

TEST_CASE("an unknown field is skipped and a missing one is defaulted", "[handover]")
{
    auto payload = vtbackend::StateWriter::payload();
    payload.varint(vtbackend::detail::fieldKey(1, vtbackend::detail::WireKind::Varint));
    payload.varint(7);
    payload.varint(vtbackend::detail::fieldKey(999, vtbackend::detail::WireKind::Varint));
    payload.varint(5);
    payload.varint(vtbackend::detail::fieldKey(998, vtbackend::detail::WireKind::Framed));
    payload.string("a field of a later release");
    auto const decoded = decodeHandover(framed(std::move(payload).take(), HandoverFormatVersion));
    REQUIRE(decoded.has_value());
    CHECK(decoded->daemonInstance == 7);
    CHECK(decoded->fromRelease.empty());
    CHECK(decoded->sessions.empty());
    CHECK(decoded->lease.isNone());
    // An image from before the upgrade generation: its writer had completed none.
    CHECK(decoded->upgradeGeneration == 0);
}

TEST_CASE("a handover image carrying release records still restores", "[handover][golden]")
{
    // Field 14 held the follow model's release records, one list of framed records, one per workspace (field
    // 1 the workspace, field 2 why: 0 Closed, 1 SwitchedAway). The model is gone; an image from the daemon
    // that wrote them is still read, the records skipped, and everything else in it kept.
    auto const record = [](uint64_t workspace, uint64_t why) {
        auto fields = vtbackend::StateWriter::payload();
        fields.varint(vtbackend::detail::fieldKey(1, vtbackend::detail::WireKind::Varint));
        fields.varint(workspace);
        fields.varint(vtbackend::detail::fieldKey(2, vtbackend::detail::WireKind::Varint));
        fields.varint(why);
        return std::move(fields).take();
    };
    auto payload = vtbackend::StateWriter::payload();
    payload.varint(vtbackend::detail::fieldKey(1, vtbackend::detail::WireKind::Varint));
    payload.varint(7);
    payload.varint(vtbackend::detail::fieldKey(2, vtbackend::detail::WireKind::Framed));
    payload.string("0.9.0");
    auto releases = vtbackend::StateWriter::payload();
    releases.bytes(record(3, 1));
    releases.bytes(record(5, 0));
    payload.varint(vtbackend::detail::fieldKey(14, vtbackend::detail::WireKind::Framed));
    payload.bytes(std::move(releases).take());
    payload.varint(vtbackend::detail::fieldKey(11, vtbackend::detail::WireKind::Varint));
    payload.varint(4);
    auto const decoded = decodeHandover(framed(std::move(payload).take(), HandoverFormatVersion));
    REQUIRE(decoded.has_value());
    CHECK(decoded->daemonInstance == 7);
    CHECK(decoded->fromRelease == "0.9.0");
    CHECK(decoded->upgradeGeneration == 4);
}

TEST_CASE("a field of the wrong kind is Malformed", "[handover]")
{
    auto payload = vtbackend::StateWriter::payload();
    payload.varint(vtbackend::detail::fieldKey(1, vtbackend::detail::WireKind::Framed));
    payload.string("not a number");
    auto const decoded = decodeHandover(framed(std::move(payload).take(), HandoverFormatVersion));
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == HandoverError::Malformed);
}

TEST_CASE("a pane tree deeper than the decoder allows is Malformed", "[handover]")
{
    // Every node claims one child: a chain as long as the stream makes it.
    auto nodes = vtbackend::StateWriter::payload();
    constexpr auto Depth = 5000;
    for (auto const index: std::views::iota(0, Depth))
    {
        (void) index;
        auto node = vtbackend::StateWriter::payload();
        node.varint(vtbackend::detail::fieldKey(5, vtbackend::detail::WireKind::Varint));
        node.varint(1);
        nodes.bytes(std::move(node).take());
    }
    auto root = vtbackend::StateWriter::payload();
    root.varint(vtbackend::detail::fieldKey(1, vtbackend::detail::WireKind::Framed));
    root.bytes(std::move(nodes).take());
    auto tab = vtbackend::StateWriter::payload();
    tab.varint(vtbackend::detail::fieldKey(2, vtbackend::detail::WireKind::Framed));
    tab.bytes(std::move(root).take());
    // The workspaces and tabs are sequences, each element framed.
    auto tabs = vtbackend::StateWriter::payload();
    tabs.bytes(std::move(tab).take());
    auto workspace = vtbackend::StateWriter::payload();
    workspace.varint(vtbackend::detail::fieldKey(2, vtbackend::detail::WireKind::Framed));
    workspace.bytes(std::move(tabs).take());
    auto workspaces = vtbackend::StateWriter::payload();
    workspaces.bytes(std::move(workspace).take());
    auto tree = vtbackend::StateWriter::payload();
    tree.varint(vtbackend::detail::fieldKey(1, vtbackend::detail::WireKind::Framed));
    tree.bytes(std::move(workspaces).take());
    auto image = vtbackend::StateWriter::payload();
    image.varint(vtbackend::detail::fieldKey(3, vtbackend::detail::WireKind::Framed));
    image.bytes(std::move(tree).take());
    auto const decoded = decodeHandover(framed(std::move(image).take(), HandoverFormatVersion));
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == HandoverError::Malformed);
}

TEST_CASE("a truncated handover stream is refused at every length", "[handover]")
{
    auto const stream = encodeHandover(sampleImage());
    for (auto const length: std::views::iota(size_t { 0 }, stream.size()))
    {
        auto const decoded = decodeHandover(std::span { stream }.first(length));
        INFO("length " << length);
        REQUIRE_FALSE(decoded.has_value());
    }
}

TEST_CASE("a mutated handover stream is refused or decoded, never fatal", "[handover]")
{
    auto const stream = encodeHandover(sampleImage());
    auto random = SplitMix64 { 0x5EEDULL };
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 1 };
    constexpr auto Rounds = 3000;
    for (auto const round: std::views::iota(0, Rounds))
    {
        (void) round;
        if (std::chrono::steady_clock::now() > deadline)
            break;
        auto mutated = stream;
        auto const flips = 1 + (random.next() % 4);
        for (auto const flip: std::views::iota(uint64_t { 0 }, flips))
        {
            (void) flip;
            mutated.at(random.next() % mutated.size()) = static_cast<uint8_t>(random.next());
        }
        // The outcome is free; surviving it (under the sanitizers) is the check.
        auto const decoded = decodeHandover(mutated);
        if (decoded)
            (void) encodeHandover(*decoded);
    }
    SUCCEED();
}

namespace
{

/// An image whose every name-bearing field is set: the workspace's, the tab's and the pane's name, the tab's
/// runtime title, and the session's name. Recorded as testing::RecordedHandoverNames.
[[nodiscard]] HandoverImage namedImage()
{
    auto tab = vtworkspace::TabState {};
    tab.id = vtworkspace::TabId { 2 };
    tab.root = leaf(3, 1);
    tab.view.activePane = vtworkspace::PaneId { 3 };
    tab.view.recentPanes = { vtworkspace::PaneId { 3 } };
    tab.view.title = "edited {Cwd}";
    auto workspace = vtworkspace::WorkspaceState {};
    workspace.id = vtworkspace::WorkspaceId { 1 };
    workspace.tabs = { tab };
    workspace.activeTabIndex = 0;

    auto session = HandoverSession {};
    session.id = vtworkspace::SessionId { 1 };
    session.uuid = uuidOf(0xC3);
    session.name = "logs";
    session.pty.pageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };

    auto image = HandoverImage {};
    image.fromRelease = "0.8.1";
    image.tree.workspaces = { workspace };
    image.modelIds = { vtworkspace::WorkspaceId { 2 }, vtworkspace::TabId { 3 }, vtworkspace::PaneId { 4 } };
    image.nextSessionId = vtworkspace::SessionId { 2 };
    image.defaultWorkspace = 1;
    image.objects.workspaces = { BoundObject { .id = 1, .uuid = uuidOf(0xC1) } };
    image.objects.tabs = { BoundObject { .id = 2, .uuid = uuidOf(0xC2) } };
    image.objects.panes = { BoundObject { .id = 1, .uuid = uuidOf(0xC3) } };
    image.objects.names = {
        NamedObject { .ref = { vtcontrol::ObjectKind::Workspace, uuidOf(0xC1) }, .name = "ops" },
        NamedObject { .ref = { vtcontrol::ObjectKind::Tab, uuidOf(0xC2) }, .name = "build" },
        NamedObject { .ref = { vtcontrol::ObjectKind::Pane, uuidOf(0xC3) }, .name = "logs" },
    };
    image.sessions = { session };
    return image;
}

} // namespace

TEST_CASE("the handover stream of a named workspace, tab and pane is pinned", "[handover][encoding-pin]")
{
    auto const image = namedImage();
    auto const encoded = encodeHandover(image);
    if (testing::isFixtureUpdateRequested(core::LiveEnvironment {}))
    {
        std::cout << testing::spelledFixture("RecordedHandoverNames", std::as_bytes(std::span { encoded }));
        FAIL("printed RecordedHandoverNames instead of comparing; paste it into RecordedNameEncodings.hpp");
    }
    CHECK(std::ranges::equal(encoded, testing::RecordedHandoverNames));

    auto const decoded = decodeHandover(testing::RecordedHandoverNames);
    REQUIRE(decoded.has_value());
    CHECK(decoded->fromRelease == image.fromRelease);
    CHECK(decoded->defaultWorkspace == image.defaultWorkspace);
    CHECK(decoded->nextSessionId == image.nextSessionId);
    REQUIRE(decoded->tree.workspaces.size() == 1);
    auto const& workspace = decoded->tree.workspaces.at(0);
    CHECK(workspace.id == vtworkspace::WorkspaceId { 1 });
    CHECK(workspace.activeTabIndex == 0);
    REQUIRE(workspace.tabs.size() == 1);
    auto const& tab = workspace.tabs.at(0);
    CHECK(tab.id == vtworkspace::TabId { 2 });
    CHECK(tab.root.id == vtworkspace::PaneId { 3 });
    CHECK(tab.root.session == vtworkspace::SessionId { 1 });
    CHECK(tab.view.activePane == vtworkspace::PaneId { 3 });
    CHECK(tab.view.recentPanes == image.tree.workspaces.at(0).tabs.at(0).view.recentPanes);
    CHECK(tab.view.title == std::optional<std::string> { "edited {Cwd}" });
    CHECK(decoded->objects.workspaces.at(0).uuid == uuidOf(0xC1));
    CHECK(decoded->objects.tabs.at(0).uuid == uuidOf(0xC2));
    CHECK(decoded->objects.panes.at(0).uuid == uuidOf(0xC3));
    REQUIRE(decoded->objects.names.size() == image.objects.names.size());
    for (auto const index: std::views::iota(size_t { 0 }, image.objects.names.size()))
    {
        CHECK(decoded->objects.names.at(index).ref == image.objects.names.at(index).ref);
        CHECK(decoded->objects.names.at(index).name == image.objects.names.at(index).name);
    }
    REQUIRE(decoded->sessions.size() == 1);
    CHECK(decoded->sessions.at(0).id == vtworkspace::SessionId { 1 });
    CHECK(decoded->sessions.at(0).uuid == uuidOf(0xC3));
    CHECK(decoded->sessions.at(0).name == "logs");
}

TEST_CASE("a workspace's colour crosses a handover, and having none does too", "[handover][workspace-color]")
{
    auto image = sampleImage();
    auto plain = image.tree.workspaces.front();
    plain.id = vtworkspace::WorkspaceId { 11 };
    image.tree.workspaces.front().color = vtbackend::RGBColor { 0x7A3E9DU };
    image.tree.workspaces.push_back(plain);

    auto const decoded = decodeHandover(encodeHandover(image));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->tree.workspaces.size() == 2);
    CHECK(decoded->tree.workspaces.at(0).color == vtbackend::RGBColor { 0x7A3E9DU });
    CHECK_FALSE(decoded->tree.workspaces.at(1).color.has_value());
}

TEST_CASE("a handover image from before the workspace colour carries none", "[handover][workspace-color]")
{
    // Written by a daemon that knew no workspace colour: its workspaces come across without one.
    auto const decoded = decodeHandover(testing::RecordedHandoverNames);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->tree.workspaces.size() == 1);
    CHECK_FALSE(decoded->tree.workspaces.front().color.has_value());
    // And a workspace without one is written as that daemon wrote it, so that daemon reads it back.
    CHECK(std::ranges::equal(encodeHandover(namedImage()), testing::RecordedHandoverNames));
}

namespace
{

/// @return A handover stream of one workspace (id 9) whose colour field, field 5, holds @p colorField.
[[nodiscard]] std::vector<uint8_t> imageWithWorkspaceColorField(std::vector<uint8_t> const& colorField)
{
    using vtbackend::detail::fieldKey;
    using vtbackend::detail::WireKind;
    auto workspace = vtbackend::StateWriter::payload();
    workspace.varint(fieldKey(1, WireKind::Varint));
    workspace.varint(9);
    workspace.varint(fieldKey(5, WireKind::Framed));
    workspace.bytes(colorField);
    auto workspaces = vtbackend::StateWriter::payload();
    workspaces.bytes(std::move(workspace).take());
    auto tree = vtbackend::StateWriter::payload();
    tree.varint(fieldKey(1, WireKind::Framed));
    tree.bytes(std::move(workspaces).take());
    auto payload = vtbackend::StateWriter::payload();
    payload.varint(fieldKey(3, WireKind::Framed));
    payload.bytes(std::move(tree).take());
    return framed(std::move(payload).take(), HandoverFormatVersion);
}

} // namespace

TEST_CASE("a workspace's colour cut short in a handover image is refused", "[handover][workspace-color]")
{
    using vtbackend::detail::fieldKey;
    using vtbackend::detail::WireKind;
    // The colour record: red, green and blue, one varint field each.
    auto const rgb = std::vector<uint8_t> { static_cast<uint8_t>(fieldKey(1, WireKind::Varint)), 0x7A,
                                            static_cast<uint8_t>(fieldKey(2, WireKind::Varint)), 0x3E,
                                            static_cast<uint8_t>(fieldKey(3, WireKind::Varint)), 0x1D };

    SECTION("whole, for contrast")
    {
        auto field = std::vector<uint8_t> { 1, static_cast<uint8_t>(rgb.size()) };
        field.insert(field.end(), rgb.begin(), rgb.end());
        auto const decoded = decodeHandover(imageWithWorkspaceColorField(field));
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->tree.workspaces.size() == 1);
        CHECK(decoded->tree.workspaces.front().color == vtbackend::RGBColor { 0x7A3E1DU });
    }
    SECTION("present, and nothing after")
    {
        CHECK(decodeHandover(imageWithWorkspaceColorField({ 1 })).error() == HandoverError::Truncated);
    }
    SECTION("present, its record cut after two of its six bytes")
    {
        auto const field = std::vector<uint8_t> { 1, static_cast<uint8_t>(rgb.size()), rgb[0], rgb[1] };
        CHECK(decodeHandover(imageWithWorkspaceColorField(field)).error() == HandoverError::Truncated);
    }
}

TEST_CASE("the attention ledger round-trips through the handover image", "[handover][daemon-attention]")
{
    using vthost::proto::FinishedCommand;
    using vthost::proto::OutputActivity;
    using vthost::proto::PaneBell;
    auto image = sampleImage();
    // Before the epoch too: a wall clock set back is still an instant the image keeps.
    auto const finishedAt =
        std::chrono::system_clock::time_point { std::chrono::milliseconds { 1'790'000'000'123 } };
    image.attention = HandoverAttention {
        .lastSerial = 41,
        .panes = {
            HandoverPaneAttention { .session = 3, .attention = PaneAttention {} },
            HandoverPaneAttention {
                .session = 5,
                .attention = PaneAttention { .activity = OutputActivity::Output,
                                             .activitySerial = 38,
                                             .bell = PaneBell::Rang,
                                             .bellSerial = 40,
                                             .finished = FinishedCommand { .exitCode = -2,
                                                                           .durationMs = 1500,
                                                                           .commandLine = "make all" },
                                             .finishedSerial = 41,
                                             .finishedAt = finishedAt,
                                             .seenStatusGeneration = 9 } },
            HandoverPaneAttention {
                .session = 8,
                .attention = PaneAttention { .finished = FinishedCommand {},
                                             .finishedSerial = 12,
                                             .finishedAt = std::chrono::system_clock::time_point {
                                                 std::chrono::seconds { -5 } } } },
        },
    };

    auto const decoded = decodeHandover(encodeHandover(image));
    REQUIRE(decoded.has_value());
    CHECK(decoded->attention == image.attention);

    // A host that raised nothing hands over an empty ledger.
    auto older = sampleImage();
    REQUIRE(older.attention == HandoverAttention {});
    auto const decodedOlder = decodeHandover(encodeHandover(older));
    REQUIRE(decodedOlder.has_value());
    CHECK(decodedOlder->attention == HandoverAttention {});
}
