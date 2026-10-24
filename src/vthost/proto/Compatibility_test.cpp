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
#include <vthost/TappingPty.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/proto/ReleasedVersions.hpp>
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

/// Decodes the daemon's answer into @p out until the attach snapshot landed or the stream ended.
Task<void> decodeAttach(core::net::ISocket* client, std::vector<proto::DecodedFrame>* out)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(4096);
    while (!std::ranges::any_of(*out, endsSnapshotRun))
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
    co_await core::net::testing::anyOf(decodeAttach(client, out),
                                       core::net::testing::sleepFor(loop, AttachBudget));
    if (std::ranges::any_of(*out, endsSnapshotRun))
        *end = ExchangeEnd::Attached;
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
    for (auto const line: frame | std::views::chunk(16))
    {
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
        }
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
