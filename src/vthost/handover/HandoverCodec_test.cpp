// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/StateFields.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <ranges>

#include <vthost/SessionSettings.hpp>
#include <vthost/handover/HandoverCodec.hpp>

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
    session.label = "shell";
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
    auto window = vtworkspace::WindowState {};
    window.id = vtworkspace::WindowId { 9 };
    window.tabs = { tab };
    window.activeTabIndex = 0;
    image.tree.windows = { window };
    image.modelIds = { vtworkspace::WindowId { 10 }, vtworkspace::TabId { 6 }, vtworkspace::PaneId { 4 } };
    image.nextSessionId = vtworkspace::SessionId { 3 };
    image.objects.windows = { BoundObject { .id = 9, .uuid = uuidOf(0xA1) } };
    image.objects.tabs = { BoundObject { .id = 5, .uuid = uuidOf(0xA2) } };
    image.objects.panes = { BoundObject { .id = 1, .uuid = uuidOf(0xA3) },
                            BoundObject { .id = 2, .uuid = uuidOf(0xA4) } };
    image.objects.labels = { LabeledObject { .ref = { vtcontrol::ObjectKind::Tab, uuidOf(0xA2) },
                                             .label = "work" } };
    image.sessions = { sessionOf(1), sessionOf(2) };
    image.listeners = { InheritedListener { .name = "control", .fd = 20 },
                        InheritedListener { .name = "native-tcp",
                                            .fd = 21,
                                            .tlsIdentity =
                                                TlsIdentity { .certPem = "-----BEGIN CERTIFICATE-----",
                                                              .keyPem = "-----BEGIN PRIVATE KEY-----" } } };
    image.leaseFd = 30;
    image.defaultColorPreference = vtbackend::ColorPreference::Dark;
    return image;
}

/// Compares what the codec promises to carry.
void requireSame(HandoverImage const& actual, HandoverImage const& expected)
{
    CHECK(actual.daemonInstance == expected.daemonInstance);
    CHECK(actual.fromRelease == expected.fromRelease);
    CHECK(actual.nextSessionId == expected.nextSessionId);
    CHECK(actual.leaseFd == expected.leaseFd);
    CHECK(actual.modelIds.nextWindow == expected.modelIds.nextWindow);
    CHECK(actual.modelIds.nextTab == expected.modelIds.nextTab);
    CHECK(actual.modelIds.nextPane == expected.modelIds.nextPane);
    REQUIRE(actual.tree.windows.size() == expected.tree.windows.size());
    auto const& window = actual.tree.windows.at(0);
    auto const& wanted = expected.tree.windows.at(0);
    CHECK(window.id == wanted.id);
    CHECK(window.activeTabIndex == wanted.activeTabIndex);
    CHECK(window.previousActiveTabIndex == wanted.previousActiveTabIndex);
    REQUIRE(window.tabs.size() == 1);
    auto const& tab = window.tabs.at(0);
    CHECK(tab.id == wanted.tabs.at(0).id);
    CHECK(tab.root.split == vtworkspace::SplitState::Vertical);
    CHECK(tab.root.ratio == 0.25);
    REQUIRE(tab.root.children.size() == 2);
    CHECK(tab.root.children.at(1).session == vtworkspace::SessionId { 2 });
    CHECK(tab.view.zoom == vtworkspace::ZoomState::Zoomed);
    CHECK(tab.view.recentPanes == wanted.tabs.at(0).view.recentPanes);
    CHECK(tab.view.title == std::optional<std::string> { "build" });
    CHECK(tab.view.colors == wanted.tabs.at(0).view.colors);
    CHECK(actual.objects.windows.size() == 1);
    CHECK(actual.objects.panes.at(1).uuid == uuidOf(0xA4));
    CHECK(actual.objects.labels.at(0).label == "work");
    CHECK(actual.objects.labels.at(0).ref == expected.objects.labels.at(0).ref);
    CHECK(actual.defaultColorPreference == expected.defaultColorPreference);
    CHECK(actual.listeners.size() == 2);
    CHECK(actual.listeners.at(1).name == "native-tcp");
    CHECK(actual.listeners.at(1).fd == 21);
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
        CHECK(got.label == want.label);
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
    CHECK(decoded->leaseFd == -1);
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
    // The windows and tabs are sequences, each element framed.
    auto tabs = vtbackend::StateWriter::payload();
    tabs.bytes(std::move(tab).take());
    auto window = vtbackend::StateWriter::payload();
    window.varint(vtbackend::detail::fieldKey(2, vtbackend::detail::WireKind::Framed));
    window.bytes(std::move(tabs).take());
    auto windows = vtbackend::StateWriter::payload();
    windows.bytes(std::move(window).take());
    auto tree = vtbackend::StateWriter::payload();
    tree.varint(vtbackend::detail::fieldKey(1, vtbackend::detail::WireKind::Framed));
    tree.bytes(std::move(windows).take());
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
