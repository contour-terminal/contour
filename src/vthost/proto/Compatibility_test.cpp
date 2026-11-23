// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshot.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/ControlVersions.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/proto/PduTrace.hpp>
#include <vthost/proto/ReleasedVersions.hpp>
#include <vthost/testing/FakeDaemonControl.hpp>
#include <vthost/testing/PduExamples.hpp>
#include <vthost/testing/RecordedTranscripts.hpp>
#include <vthost/testing/TestHelpers.hpp>

using core::async::Task;
namespace proto = vthost::proto;

namespace
{

// ---------------------------------------------------------------------------
// The release check: what metainfo.xml says was released.

/// @param text Some XML.
/// @return @p text without its comments.
[[nodiscard]] std::string withoutComments(std::string_view text)
{
    auto kept = std::string {};
    auto first = true;
    for (auto const piece: std::views::split(text, std::string_view { "<!--" }))
    {
        auto const chunk = std::string_view { piece.begin(), piece.end() };
        if (std::exchange(first, false))
        {
            kept += chunk;
            continue;
        }
        if (auto const end = chunk.find("-->"); end != std::string_view::npos)
            kept += chunk.substr(end + 3);
    }
    return kept;
}

/// @param tag The inside of one start tag: its name and attributes.
/// @param name The attribute to look for.
/// @return Its value, or std::nullopt when @p tag has none.
[[nodiscard]] std::optional<std::string> attribute(std::string_view tag, std::string_view name)
{
    auto const prefix = std::format("{}=\"", name);
    for (auto const piece: std::views::split(tag, ' '))
    {
        auto const token = std::string_view { piece.begin(), piece.end() };
        if (!token.starts_with(prefix))
            continue;
        auto const value = token.substr(prefix.size());
        return std::string { value.substr(0, value.find('"')) };
    }
    return std::nullopt;
}

/// The newest release `metainfo.xml` names: the first `<release>` that carries a `date=`, as the release
/// tooling stamps one on the entry it cuts, its attributes separated by spaces (the tag name by any
/// whitespace). An undated entry is a release
/// still in development, and a commented-out one is a template.
/// @param metainfo The file's text.
/// @return That release's version, or std::nullopt when no entry is dated.
[[nodiscard]] std::optional<std::string> newestDatedRelease(std::string_view metainfo)
{
    auto const text = withoutComments(metainfo);
    constexpr auto Name = std::string_view { "release" };
    constexpr auto Whitespace = std::string_view { " \t\r\n" };
    for (auto const piece: std::views::split(std::string_view { text }, '<'))
    {
        auto tag = std::string_view { piece.begin(), piece.end() };
        tag = tag.substr(0, tag.find('>'));
        if (!tag.starts_with(Name) || tag.size() == Name.size() || !Whitespace.contains(tag[Name.size()]))
            continue;
        // Attributes are looked up between spaces; any other whitespace separates them just as well.
        auto attributes = std::string { tag };
        std::ranges::replace_if(attributes, [=](char c) { return Whitespace.contains(c); }, ' ');
        if (!attribute(attributes, "date"))
            continue;
        return attribute(attributes, "version");
    }
    return std::nullopt;
}

/// Whether @p releases has the row the release @p newest needs. The table always ends in the one
/// UnreleasedProtocol row; before the first negotiating release it is that row alone, and after one the
/// row before it names the newest release.
/// @param newest The newest dated release in `metainfo.xml`.
/// @param releases The released protocols, oldest first.
/// @param lastBeforeNegotiation The newest release that predates negotiation (@see
///        proto::LastReleaseBeforeNegotiation), a parameter so the cases below describe their own history.
/// @return The step the release is missing, or std::nullopt when nothing is.
[[nodiscard]] std::optional<std::string> missingReleaseStep(std::string_view newest,
                                                            std::span<proto::ReleasedProtocol const> releases,
                                                            std::string_view lastBeforeNegotiation)
{
    auto const isUnreleased = [](proto::ReleasedProtocol const& row) {
        return row.release == proto::UnreleasedProtocol;
    };
    auto const endsInUnreleased = !releases.empty() && isUnreleased(releases.back())
                                  && std::ranges::count_if(releases, isUnreleased) == 1;
    auto const complete =
        newest == lastBeforeNegotiation
            ? endsInUnreleased && releases.size() == 1
            : endsInUnreleased && releases.size() >= 2 && releases[releases.size() - 2].release == newest;
    if (complete)
        return std::nullopt;
    return std::format("add a ReleasedProtocols row for {} and record its transcripts", newest);
}

/// @return `metainfo.xml` as the build tree's source holds it.
[[nodiscard]] std::string readMetainfo()
{
    auto in = std::ifstream { CONTOUR_METAINFO_PATH };
    REQUIRE(in.good());
    auto text = std::ostringstream {};
    text << in.rdbuf();
    return std::move(text).str();
}

// ---------------------------------------------------------------------------
// The recorded transcripts, served.

/// What the recorded Input is written into: the first session a host opens.
constexpr auto RecordedInputSession = uint64_t { 1 };

/// How a served exchange ended.
enum class ExchangeEnd : uint8_t
{
    StreamEnded, ///< The daemon closed the connection, or the budget ran out, before the attach landed.
    Attached,    ///< The attach snapshot landed over a connection still open.
    Answered,    ///< A control request's answer arrived.
};

/// One host and one connection to it, driven with raw bytes.
struct ServingHarness
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    vthost::SessionHost host {
        loop,
        [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&)
            -> std::unique_ptr<vtpty::Pty> { return std::make_unique<vtpty::MockPty>(size); },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual },
    };
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    std::unique_ptr<vthost::NativeSession> session = std::make_unique<vthost::NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "recorded", .index = 1 }, std::move(pair.first));
};

/// @return Whether @p frame terminates a snapshot run.
[[nodiscard]] bool endsSnapshotRun(proto::DecodedFrame const& frame)
{
    auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
    return delta != nullptr && delta->snapshot != 0 && delta->completesSnapshot();
}

/// The longest an attach waits for its snapshot before it gives up: a bound against a hang, never reached
/// by an attach that works.
constexpr auto AttachBudget = std::chrono::milliseconds { 60'000 };

/// Decodes what @p client is sent into @p out until a frame satisfying @p done arrived or the stream ended.
template <typename Done>
Task<void> decodeUntil(core::net::ISocket* client, std::vector<proto::DecodedFrame>* out, Done done)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(4096);
    while (!std::ranges::any_of(*out, done))
    {
        if (auto decoded = proto::decodePdu(buffer))
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(decoded->consumed));
            out->push_back(std::move(*decoded));
            continue;
        }
        auto const n = co_await client->read(scratch);
        if (!n.has_value() || *n == 0)
            break;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
    }
}

/// Writes @p bytes, decodes the answer into @p out until the attach snapshot landed, the stream ended or
/// AttachBudget is spent, says in @p end which it was, and closes the client end.
Task<void> attachWith(core::net::EventLoop* loop,
                      core::net::ISocket* client,
                      std::vector<std::byte> const* bytes,
                      std::vector<proto::DecodedFrame>* out,
                      ExchangeEnd* end)
{
    std::ignore = co_await client->write(std::span<std::byte const> { *bytes });
    co_await core::net::testing::anyOf(decodeUntil(client, out, endsSnapshotRun),
                                       core::net::testing::sleepFor(loop, AttachBudget));
    if (std::ranges::any_of(*out, endsSnapshotRun))
        *end = ExchangeEnd::Attached;
    client->close();
}

/// @return Whether @p frame is a ServerHello.
[[nodiscard]] bool isServerHello(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::ServerHello>(frame.pdu);
}

/// @return Whether @p frame is a DaemonStatusReply.
[[nodiscard]] bool isStatusReply(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::DaemonStatusReply>(frame.pdu);
}

/// @return Whether @p received holds a ServerHello.
[[nodiscard]] bool hasServerHello(std::vector<proto::DecodedFrame> const& received)
{
    return std::ranges::any_of(received, isServerHello);
}

/// Writes @p bytes, decodes the answer into @p out until a DaemonStatusReply arrived, the stream ended or
/// AttachBudget is spent, says in @p end which it was, and closes the client end.
Task<void> askStatusWith(core::net::EventLoop* loop,
                         core::net::ISocket* client,
                         std::vector<std::byte> const* bytes,
                         std::vector<proto::DecodedFrame>* out,
                         ExchangeEnd* end)
{
    std::ignore = co_await client->write(std::span<std::byte const> { *bytes });
    co_await core::net::testing::anyOf(decodeUntil(client, out, isStatusReply),
                                       core::net::testing::sleepFor(loop, AttachBudget));
    if (std::ranges::any_of(*out, isStatusReply))
        *end = ExchangeEnd::Answered;
    client->close();
}

/// Writes @p bytes, decodes the answer until its ServerHello arrived, the stream ended or AttachBudget is
/// spent, says what it was in @p out, and closes the client end.
Task<void> helloWith(core::net::EventLoop* loop,
                     core::net::ISocket* client,
                     std::vector<std::byte> const* bytes,
                     std::optional<proto::ServerHello>* out)
{
    std::ignore = co_await client->write(std::span<std::byte const> { *bytes });
    auto received = std::vector<proto::DecodedFrame> {};
    co_await core::net::testing::anyOf(decodeUntil(client, &received, isServerHello),
                                       core::net::testing::sleepFor(loop, AttachBudget));
    if (hasServerHello(received))
        *out = std::get<proto::ServerHello>(received.front().pdu);
    client->close();
}

/// A daemon that answers the client's first frame with @p reply, and hangs up.
Task<void> recordedDaemon(core::net::ISocket* socket, std::span<std::byte const> reply)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(4096);
    auto frame = proto::readFrame(buffer);
    while (!frame && frame.error() == proto::DecodeError::NeedMoreData)
    {
        auto const n = co_await socket->read(scratch);
        if (!n.has_value() || *n == 0)
            co_return;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
        frame = proto::readFrame(buffer);
    }
    if (frame)
        std::ignore = co_await socket->write(reply);
    socket->close();
}

/// A daemon that answers the client's n-th frame with @p replies[n], and hangs up after the last.
Task<void> scriptedDaemon(core::net::ISocket* socket, std::vector<std::span<std::byte const>> replies)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(4096);
    for (auto const reply: replies)
    {
        auto frame = proto::readFrame(buffer);
        while (!frame && frame.error() == proto::DecodeError::NeedMoreData)
        {
            auto const n = co_await socket->read(scratch);
            if (!n.has_value() || *n == 0)
            {
                socket->close();
                co_return;
            }
            buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
            frame = proto::readFrame(buffer);
        }
        if (!frame)
            break;
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(frame->consumed));
        std::ignore = co_await socket->write(reply);
    }
    socket->close();
}

/// Serves @p socket as scriptedDaemon() does, owning it for as long.
Task<void> serveScripted(std::unique_ptr<core::net::ISocket> socket,
                         std::vector<std::span<std::byte const>> replies)
{
    co_await scriptedDaemon(socket.get(), std::move(replies));
}

/// @return One end of a fresh pair, a scriptedDaemon() of @p replies serving the other on @p loop.
Task<std::expected<std::unique_ptr<core::net::ISocket>, vthost::client::ControlError>> connectScripted(
    core::net::EventLoop* loop, std::vector<std::span<std::byte const>> replies)
{
    auto pair = *core::net::testing::makeSocketPair(*loop);
    loop->spawn(serveScripted(std::move(pair.first), std::move(replies)));
    co_return std::move(pair.second);
}

/// How long a control call against a recorded daemon may take: a bound against a hang, never reached.
constexpr auto ControlBudget = std::chrono::seconds { 2 };

/// @param version A protocol version.
/// @return Its recorded transcript, if one is.
[[nodiscard]] vthost::testing::RecordedTranscript const* transcriptOf(uint32_t version)
{
    auto const found = std::ranges::find(
        vthost::testing::RecordedTranscripts, version, &vthost::testing::RecordedTranscript::version);
    return found == vthost::testing::RecordedTranscripts.end() ? nullptr : &*found;
}

/// @return Whether @p version lies in @p range.
[[nodiscard]] constexpr bool within(uint32_t version, proto::VersionRange range) noexcept
{
    return range.min <= version && version <= range.max;
}

/// @param name The constant's name.
/// @param frame One frame's worth of bytes.
/// @return Its declaration, as RecordedTranscripts.hpp spells one.
[[nodiscard]] std::string spelled(std::string_view name, std::span<std::byte const> frame)
{
    // Sixteen bytes to a literal, each escaped, so that no escape runs into the character after it.
    auto text = std::format("constexpr auto {} = recorded(", name);
    auto separator = std::string_view {};
    // Row indices rather than std::views::chunk, which the libc++ that macOS ships lacks.
    constexpr auto BytesPerLine = std::size_t { 16 };
    for (auto const row:
         std::views::iota(std::size_t { 0 }, (frame.size() + BytesPerLine - 1) / BytesPerLine))
    {
        auto const line =
            frame.subspan(row * BytesPerLine, std::min(BytesPerLine, frame.size() - (row * BytesPerLine)));
        text += std::exchange(separator, "\n                                    ");
        text += '"';
        for (auto const byte: line)
            text += std::format("\\x{:02x}", std::to_integer<unsigned>(byte));
        text += '"';
    }
    return text + ");\n";
}

/// @param serial The PDU's serial.
/// @param pdu A PDU.
/// @return Its frame, as this build writes it.
[[nodiscard]] std::vector<std::byte> frameOf(uint64_t serial, proto::DecodedPdu const& pdu)
{
    auto sink = proto::Writer {};
    proto::encodePdu(sink, serial, pdu);
    return sink.take();
}

/// The release the recorded control conversations name, both as the daemon's and as the upgrade target's.
constexpr auto RecordedControlRelease = std::string_view { "0.8.1" };
constexpr auto RecordedUpgradeImage = std::string_view { "0.8.2-0123456789abcdef" };

/// @param version The protocol version a recorded control conversation is of.
/// @return This build's control conversation, as a client and a daemon of exactly @p version write it.
[[nodiscard]] std::array<std::vector<std::byte>, 6> controlConversationOf(uint32_t version)
{
    auto const capabilities =
        proto::CapabilitySet::of({ proto::Capability::DaemonControl, proto::Capability::LiveUpgrade });
    auto const range = proto::VersionRange { .min = version, .max = version };
    return {
        frameOf(1,
                proto::ClientHello { .versions = range,
                                     .capabilities = capabilities,
                                     .role = proto::ConnectionRole::Control }),
        frameOf(1,
                proto::ServerHello { .agreedVersion = version,
                                     .versions = range,
                                     .capabilities = capabilities,
                                     .machineId = "5f1d2c3b4a596877",
                                     .hostname = "recorded-host",
                                     .releaseVersion = std::string { RecordedControlRelease } }),
        frameOf(2, proto::DaemonStatusRequest {}),
        frameOf(2,
                proto::DaemonStatusReply { .releaseVersion = std::string { RecordedControlRelease },
                                           .processId = 4711,
                                           .uptimeSeconds = 90,
                                           .socketPath = "/run/contour/control" }),
        frameOf(2, proto::UpgradeDaemon { .imageId = std::string { RecordedUpgradeImage } }),
        frameOf(2, proto::UpgradeDaemonReply {}),
    };
}

/// @param transcript A recorded control conversation.
/// @return Its frames, in the order controlConversationOf() returns them.
[[nodiscard]] std::array<std::span<std::byte const>, 6> framesOf(
    vthost::testing::RecordedControlTranscript const& transcript)
{
    return { transcript.clientHello, transcript.serverHello,    transcript.statusRequest,
             transcript.statusReply, transcript.upgradeRequest, transcript.upgradeReply };
}

} // namespace

TEST_CASE("this build still speaks the protocol of the previous two releases", "[vthost][negotiation]")
{
    CHECK(proto::SupportedVersions.min <= proto::compatibilityFloor(proto::ReleasedProtocols));
}

TEST_CASE("the compatibility floor is two releases back, or the oldest", "[vthost][negotiation]")
{
    // Each table as ReleasedProtocols holds it: the releases cut so far, then the build under development.
    auto const build = proto::ReleasedProtocol { .release = proto::UnreleasedProtocol, .versions = { 5, 7 } };
    auto const releases = std::array {
        proto::ReleasedProtocol { .release = "1.0.0", .versions = { .min = 4, .max = 4 } },
        proto::ReleasedProtocol { .release = "1.1.0", .versions = { .min = 4, .max = 5 } },
        proto::ReleasedProtocol { .release = "1.2.0", .versions = { .min = 4, .max = 6 } },
    };
    auto const floorAfter = [&](std::size_t cut) {
        auto table = std::vector<proto::ReleasedProtocol>(releases.begin(), releases.begin() + cut);
        table.push_back(build);
        return proto::compatibilityFloor(table);
    };
    CHECK(floorAfter(0) == 7); // Nothing released: the build answers only to itself.
    CHECK(floorAfter(1) == 4); // 1.0.0 is within the window.
    CHECK(floorAfter(2) == 4); // 1.0.0 and 1.1.0 are the previous two.
    CHECK(floorAfter(3) == 5); // 1.1.0 and 1.2.0 are; 1.0.0 has left the window.
}

TEST_CASE("every dated release since negotiation has a ReleasedProtocols row",
          "[vthost][negotiation][release]")
{
    auto const newest = newestDatedRelease(readMetainfo());
    REQUIRE(newest.has_value());
    if (auto const missing =
            missingReleaseStep(*newest, proto::ReleasedProtocols, proto::LastReleaseBeforeNegotiation))
        FAIL(*missing);
}

TEST_CASE("the release check reads metainfo.xml as the release tooling writes it",
          "[vthost][negotiation][release]")
{
    // The history these cases describe: 0.7.0 is the last release before negotiation, 0.8.0 the first with
    // it.
    constexpr auto FixtureLastBeforeNegotiation = std::string_view { "0.7.0" };
    constexpr auto Development = std::string_view {
        R"(<releases>
    <!-- unreleased: TODO: Don't forget to add date attribute to it. -->
    <!--
    <release version="x.y.z" urgency="medium" date="2099-01-01">
    </release>
    -->
    <release version="0.8.0" urgency="medium" type="development">
      <description><p>Not yet.</p></description>
    </release>
    <release version="0.7.0" urgency="medium" date="2026-08-17">
    </release>
    <release version="0.6.3" urgency="medium" date="2026-04-09">
    </release>
  </releases>)"
    };
    constexpr auto Cut = std::string_view {
        R"(<releases>
    <release version="0.8.0" urgency="medium" date="2026-11-02">
    </release>
    <release version="0.7.0" urgency="medium" date="2026-08-17">
    </release>
  </releases>)"
    };
    constexpr auto Undated = std::string_view {
        R"(<releases>
    <release version="0.8.0" urgency="medium" type="development">
    </release>
  </releases>)"
    };

    SECTION("undated and commented-out entries are skipped; the first dated one wins")
    {
        CHECK(newestDatedRelease(Development) == "0.7.0");
        CHECK(newestDatedRelease(Cut) == "0.8.0");
    }

    SECTION("a file with no dated entry names no release")
    {
        CHECK_FALSE(newestDatedRelease(Undated).has_value());
        CHECK_FALSE(newestDatedRelease("").has_value());
    }

    SECTION("the tag name ends at any whitespace")
    {
        constexpr auto Wrapped = std::string_view {
            "<releases>\n<release\n  version=\"0.8.0\"\n  date=\"2026-11-02\">\n</release>\n"
            "<release\tversion=\"0.7.0\"\tdate=\"2026-08-17\"></release>\n</releases>"
        };
        CHECK(newestDatedRelease(Wrapped) == "0.8.0");
        CHECK(newestDatedRelease(Wrapped.substr(Wrapped.find("</release>"))) == "0.7.0");
    }

    auto const unreleased =
        proto::ReleasedProtocol { .release = proto::UnreleasedProtocol, .versions = { 4, 5 } };
    auto const release080 = proto::ReleasedProtocol { .release = "0.8.0", .versions = { 4, 4 } };

    SECTION("before the first negotiating release only the unreleased row may stand")
    {
        CHECK_FALSE(
            missingReleaseStep("0.7.0", std::array { unreleased }, FixtureLastBeforeNegotiation).has_value());
        CHECK(
            missingReleaseStep("0.7.0", std::array { release080 }, FixtureLastBeforeNegotiation).has_value());
        CHECK(missingReleaseStep("0.7.0", std::array { release080, unreleased }, FixtureLastBeforeNegotiation)
                  .has_value());
    }

    SECTION("a cut release needs its own row, just before the one unreleased row")
    {
        CHECK(missingReleaseStep(
                  *newestDatedRelease(Cut), std::array { unreleased }, FixtureLastBeforeNegotiation)
              == "add a ReleasedProtocols row for 0.8.0 and record its transcripts");

        auto const recorded = std::array { release080, unreleased };
        CHECK_FALSE(missingReleaseStep("0.8.0", recorded, FixtureLastBeforeNegotiation).has_value());
        CHECK(missingReleaseStep("0.9.0", recorded, FixtureLastBeforeNegotiation)
              == "add a ReleasedProtocols row for 0.9.0 and record its transcripts");

        // The build under development must stay the last row, and the only unreleased one.
        CHECK(
            missingReleaseStep("0.8.0", std::array { release080 }, FixtureLastBeforeNegotiation).has_value());
        CHECK(missingReleaseStep("0.8.0", std::array { unreleased, release080 }, FixtureLastBeforeNegotiation)
                  .has_value());
        CHECK(missingReleaseStep(
                  "0.8.0", std::array { unreleased, release080, unreleased }, FixtureLastBeforeNegotiation)
                  .has_value());
    }
}

TEST_CASE("every released and every supported version has a recorded transcript", "[vthost][negotiation]")
{
    auto versions = std::vector<uint32_t> {};
    for (auto const& row: proto::ReleasedProtocols)
        std::ranges::copy(std::views::iota(row.versions.min, row.versions.max + 1),
                          std::back_inserter(versions));
    std::ranges::copy(std::views::iota(proto::SupportedVersions.min, proto::SupportedVersions.max + 1),
                      std::back_inserter(versions));
    for (auto const version: versions)
    {
        INFO("protocol version " << version);
        CHECK(transcriptOf(version) != nullptr);
    }
}

TEST_CASE("every recorded transcript of a supported version is served", "[vthost][negotiation]")
{
    auto const supported = [](vthost::testing::RecordedTranscript const& transcript) {
        return within(transcript.version, proto::SupportedVersions);
    };
    for (auto const& transcript: vthost::testing::RecordedTranscripts | std::views::filter(supported))
    {
        INFO("protocol version " << transcript.version);

        SECTION(std::format("a client of version {} attaches, resizes and types", transcript.version))
        {
            auto h = ServingHarness {};
            REQUIRE(vthost::testing::newSession(h.host).value == RecordedInputSession);
            auto bytes = std::vector<std::byte> {};
            for (auto const frame: { transcript.clientHello, transcript.resizeRequest, transcript.input })
                bytes.insert(bytes.end(), frame.begin(), frame.end());
            auto received = std::vector<proto::DecodedFrame> {};
            auto end = ExchangeEnd::StreamEnded;
            h.loop.blockOn(core::net::testing::allOf(
                h.session->run(), attachWith(&h.loop, h.pair.second.get(), &bytes, &received, &end)));

            CHECK(end == ExchangeEnd::Attached);
            REQUIRE(h.session->negotiated().has_value());
            CHECK(h.session->negotiated()->version == transcript.version);
            REQUIRE_FALSE(received.empty());
            auto const* hello = std::get_if<proto::ServerHello>(&received.front().pdu);
            REQUIRE(hello != nullptr);
            CHECK(hello->agreedVersion == transcript.version);
            CHECK_FALSE(std::ranges::any_of(received, [](auto const& frame) {
                return std::holds_alternative<proto::Unsupported>(frame.pdu);
            }));
            // The ResizeRequest and the Input behind the hello were served, not dropped.
            CHECK(h.host.pageSize()
                  == vtpty::PageSize { .lines = vtpty::LineCount(30), .columns = vtpty::ColumnCount(100) });
            auto& tapped = dynamic_cast<vthost::TappingPty&>(
                h.host.terminal(vtworkspace::SessionId { RecordedInputSession })->device());
            CHECK(dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer() == "ls\r");
        }

        SECTION(std::format("a daemon of version {} is attached to", transcript.version))
        {
            auto const source = core::net::makeDefaultBackend();
            auto loop = core::net::EventLoop { *source };
            auto pair = *core::net::testing::makeSocketPair(loop);
            auto* daemonSocket = pair.first.get();
            auto client = vthost::client::NativeClient { loop,
                                                         std::move(pair.second),
                                                         vthost::client::NativeClient::HandshakeOptions {},
                                                         vtbackend::CommandBlockStoreLimits {},
                                                         vthost::client::NativeClient::UpdateHandler {},
                                                         vthost::client::NativeClient::ImageHandler {},
                                                         vthost::client::NativeClient::SessionEventHandler {},
                                                         vthost::client::NativeClient::LayoutHandler {} };
            loop.blockOn(core::net::testing::allOf(client.run(),
                                                   recordedDaemon(daemonSocket, transcript.serverHello)));

            REQUIRE(client.negotiated().has_value());
            CHECK(client.negotiated()->version == transcript.version);
            CHECK_FALSE(client.refusal().has_value());
            // None of them predates the restored counts, so each hello reads as telling nothing.
            CHECK_FALSE(client.restoredCounts().has_value());
        }
    }
}

TEST_CASE("a hello that tells what a restore brought back reaches the client", "[vthost][negotiation]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = vthost::client::NativeClient { loop,
                                                 std::move(pair.second),
                                                 vthost::client::NativeClient::HandshakeOptions {},
                                                 vtbackend::CommandBlockStoreLimits {},
                                                 vthost::client::NativeClient::UpdateHandler {},
                                                 vthost::client::NativeClient::ImageHandler {},
                                                 vthost::client::NativeClient::SessionEventHandler {},
                                                 vthost::client::NativeClient::LayoutHandler {} };
    auto const hello = frameOf(1,
                               proto::ServerHello { .agreedVersion = proto::SupportedVersions.max,
                                                    .versions = proto::SupportedVersions,
                                                    .restoredWorkspaces = 2,
                                                    .restoredTabs = 7 });
    loop.blockOn(core::net::testing::allOf(client.run(), recordedDaemon(daemonSocket, hello)));

    REQUIRE(client.negotiated().has_value());
    CHECK(client.restoredCounts() == vthost::RestoredCounts { .workspaces = 2, .tabs = 7 });
}

TEST_CASE("every version a control connection spans has a recorded control conversation",
          "[vthost][negotiation][control]")
{
    for (auto const version: std::views::iota(proto::ControlVersions.min, proto::ControlVersions.max + 1))
    {
        INFO("protocol version " << version);
        CHECK(std::ranges::contains(vthost::testing::RecordedControlTranscripts,
                                    version,
                                    &vthost::testing::RecordedControlTranscript::version));
    }
}

TEST_CASE("recorded control conversations decode in full", "[vthost][negotiation][control]")
{
    for (auto const& transcript: vthost::testing::RecordedControlTranscripts)
    {
        INFO("protocol version " << transcript.version);
        for (auto const frame: framesOf(transcript))
        {
            auto const decoded = proto::decodePdu(frame);
            REQUIRE(decoded.has_value());
            CHECK(decoded->consumed == frame.size());
        }
    }
}

TEST_CASE("every control PDU is recorded at every version a control connection spans",
          "[vthost][negotiation][control]")
{
    for (auto const version: std::views::iota(proto::ControlVersions.min, proto::ControlVersions.max + 1))
    {
        INFO("protocol version " << version);
        auto const set = std::ranges::find(
            vthost::testing::RecordedControlPduSets, version, &vthost::testing::RecordedControlPdus::version);
        REQUIRE(set != vthost::testing::RecordedControlPduSets.end());
        for (auto const& row: proto::ControlPdus)
        {
            INFO("PDU " << proto::toString(row.type));
            CHECK(std::ranges::contains(set->pdus, row.type, &vthost::testing::RecordedControlPdu::type));
        }
    }
}

TEST_CASE("every recorded control PDU decodes in full to the PDU it names", "[vthost][negotiation][control]")
{
    for (auto const& set: vthost::testing::RecordedControlPduSets)
        for (auto const& recorded: set.pdus)
        {
            INFO("protocol version " << set.version << ", PDU " << proto::toString(recorded.type));
            auto const decoded = proto::decodePdu(recorded.frame);
            REQUIRE(decoded.has_value());
            CHECK(decoded->consumed == recorded.frame.size());
            CHECK(proto::typeOf(decoded->pdu) == recorded.type);
        }
}

// The tripwire: a change to how this build writes a control PDU fails here, and its author decides. A change
// older peers read past -- a trailing field they skip -- re-records the newest version's frame and keeps the
// PDU's ControlPdus row; a change they would misread sets the row to the new version.
TEST_CASE("this build writes the newest recorded control PDUs byte for byte",
          "[vthost][negotiation][control]")
{
    REQUIRE_FALSE(vthost::testing::RecordedControlPduSets.empty());
    auto const& newest = vthost::testing::RecordedControlPduSets.back();
    CHECK(newest.version == proto::ControlVersions.max);
    for (auto const& recorded: newest.pdus)
    {
        INFO("PDU " << proto::toString(recorded.type));
        auto const decoded = proto::decodePdu(recorded.frame);
        REQUIRE(decoded.has_value());
        CHECK(std::ranges::equal(frameOf(decoded->serial, decoded->pdu), recorded.frame));
    }
}

TEST_CASE("a recorded control client of every control version is served", "[vthost][negotiation][control]")
{
    for (auto const& transcript: vthost::testing::RecordedControlTranscripts)
    {
        INFO("protocol version " << transcript.version);
        auto control = vthost::testing::FakeDaemonControl {};
        auto h = ServingHarness {};
        auto pair = *core::net::testing::makeSocketPair(h.loop);
        auto session = vthost::NativeSession { h.loop,
                                               h.host,
                                               vthost::ConnectionId { .endpoint = "native", .index = 2 },
                                               std::move(pair.first),
                                               vthost::NativeSession::DefaultWriteQueueBytes,
                                               vthost::NativeSessionServices { .daemon = control } };
        auto bytes = std::vector<std::byte> {};
        for (auto const frame: { transcript.clientHello, transcript.statusRequest })
            bytes.insert(bytes.end(), frame.begin(), frame.end());
        auto received = std::vector<proto::DecodedFrame> {};
        auto end = ExchangeEnd::StreamEnded;
        h.loop.blockOn(core::net::testing::allOf(
            session.run(), askStatusWith(&h.loop, pair.second.get(), &bytes, &received, &end)));

        CHECK(end == ExchangeEnd::Answered); // the answer ended it, not the budget
        REQUIRE(session.negotiated().has_value());
        CHECK(session.negotiated()->version == transcript.version);
    }
}

TEST_CASE("a recorded daemon of every control version answers status and upgrade",
          "[vthost][negotiation][control]")
{
    for (auto const& transcript: vthost::testing::RecordedControlTranscripts)
    {
        INFO("protocol version " << transcript.version);
        auto const source = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *source };
        auto const connector = [&](std::span<std::byte const> reply) {
            return [&loop, hello = transcript.serverHello, reply] {
                return connectScripted(&loop, { hello, reply });
            };
        };

        auto statusPort =
            vthost::client::SocketDaemonControl { loop, connector(transcript.statusReply), ControlBudget };
        auto const status = statusPort.status();
        INFO((status ? std::string {} : status.error().toString()));
        REQUIRE(status.has_value());
        CHECK(status->releaseVersion == RecordedControlRelease);

        auto upgradePort =
            vthost::client::SocketDaemonControl { loop, connector(transcript.upgradeReply), ControlBudget };
        auto const upgraded = upgradePort.upgrade(std::string { RecordedUpgradeImage });
        INFO((upgraded ? std::string {} : upgraded.error().toString()));
        REQUIRE(upgraded.has_value());
        CHECK(upgraded->result == 0);
    }
}

// Prints this build's attach PDUs in the layout of RecordedTranscripts.hpp, to record a NEW protocol
// version's transcripts when the release that ships it is cut. Hidden: a shipped version's are never
// re-recorded.
TEST_CASE("dump this build's transcripts", "[.dump]")
{
    auto const capabilities = proto::CapabilitySet::of({ proto::Capability::ExtensionPermissions });
    auto const clientHello = proto::ClientHello {
        .versions = proto::SupportedVersions,
        .capabilities = capabilities,
        .token = {},
        .sessionSettings = proto::WireSessionSettings { .historyLineCount = 1000,
                                                        .wordDelimiters = " ./\\",
                                                        .frozenModes = { { .mode = 2026, .frozenAs = 0 } } },
    };
    auto const serverHello = proto::ServerHello { .agreedVersion = proto::SupportedVersions.max,
                                                  .versions = proto::SupportedVersions,
                                                  .capabilities = capabilities,
                                                  .machineId = "5f1d2c3b4a596877",
                                                  .hostname = "recorded-host" };
    auto const resize = proto::ResizeRequest { .columns = 100, .lines = 30 };
    auto const input = proto::Input { .session = RecordedInputSession,
                                      .data = { std::byte { 'l' }, std::byte { 's' }, std::byte { '\r' } } };

    std::cout << spelled("ClientHello", frameOf(1, clientHello))
              << spelled("ServerHello", frameOf(1, serverHello))
              << spelled("ResizeRequest", frameOf(2, resize)) << spelled("Input", frameOf(3, input));
}

// Prints this build's control conversation at each version a control connection spans, in the layout of
// RecordedTranscripts.hpp, to record a NEW version's when the release that ships it is cut. Hidden, as above.
TEST_CASE("dump this build's control conversations", "[.dump]")
{
    constexpr auto Names = std::array { "ClientHello",       "ServerHello",   "DaemonStatusRequest",
                                        "DaemonStatusReply", "UpgradeDaemon", "UpgradeDaemonReply" };
    for (auto const version: std::views::iota(proto::ControlVersions.min, proto::ControlVersions.max + 1))
    {
        std::cout << std::format("// protocol version {}\n", version);
        auto const frames = controlConversationOf(version);
        for (auto const& [name, frame]: std::views::zip(Names, frames))
            std::cout << spelled(name, frame);
    }
}

// Prints one frame of every PDU in proto::ControlPdus, every optional field stated (testing/PduExamples.hpp),
// in the layout of RecordedTranscripts.hpp. Hidden, as above.
TEST_CASE("dump this build's control PDUs", "[.dump]")
{
    for (auto const& [type, pdu]: vthost::testing::everyAlternative())
        if (std::ranges::contains(proto::ControlPdus, type, &proto::ControlPdu::type))
            std::cout << spelled(proto::toString(type), frameOf(1, pdu));
}

TEST_CASE("the summary of a restore is told once, to the first Attach connection",
          "[vthost][daemon][restore]")
{
    // Two restored workspaces with three and four single-pane tabs.
    auto snapshot = vthost::WorkspaceSnapshot {};
    for (auto const tabs: { 3, 4 })
    {
        auto workspace = vthost::WorkspaceLayoutSnapshot {};
        std::ranges::generate_n(
            std::back_inserter(workspace.tabs), tabs, [] { return vthost::TabSnapshot {}; });
        snapshot.workspaces.push_back(std::move(workspace));
    }
    auto source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = vthost::SessionHost {
        loop,
        [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&)
            -> std::unique_ptr<vtpty::Pty> { return std::make_unique<vtpty::MockPty>(size); },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                    .restore = vthost::WorkspaceRestore { .snapshot = std::move(snapshot) } },
    };
    REQUIRE(host.restoreOutcome() == vthost::RestoreOutcome::Realized);

    auto connections = std::size_t { 0 };
    auto const helloOf = [&](proto::ConnectionRole role) {
        auto const bytes =
            frameOf(1, proto::ClientHello { .versions = proto::SupportedVersions, .role = role });
        auto pair = *core::net::testing::makeSocketPair(loop);
        auto session =
            vthost::NativeSession { loop,
                                    host,
                                    vthost::ConnectionId { .endpoint = "restore", .index = ++connections },
                                    std::move(pair.first) };
        auto hello = std::optional<proto::ServerHello> {};
        loop.blockOn(
            core::net::testing::allOf(session.run(), helloWith(&loop, pair.second.get(), &bytes, &hello)));
        REQUIRE(hello.has_value());
        return std::pair { hello->restoredWorkspaces, hello->restoredTabs };
    };
    using Counts = std::pair<uint32_t, uint32_t>;

    // A probe or control caller is told nothing and leaves the summary for the first client that shows
    // workspaces.
    CHECK(helloOf(proto::ConnectionRole::Control) == Counts { 0, 0 });
    CHECK(helloOf(proto::ConnectionRole::Attach) == Counts { 2, 7 });
    // A second client, and a reconnect of the first, read zeros.
    CHECK(helloOf(proto::ConnectionRole::Attach) == Counts { 0, 0 });
    CHECK(helloOf(proto::ConnectionRole::Attach) == Counts { 0, 0 });
    CHECK(helloOf(proto::ConnectionRole::Control) == Counts { 0, 0 });
}
