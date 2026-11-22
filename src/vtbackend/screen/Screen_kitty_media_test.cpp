// SPDX-License-Identifier: Apache-2.0
//
// Kitty file media (`t=f`, `t=t`) on a terminal: read through the injected source only for bytes a local
// process wrote to the PTY, and answered with one refusal for every failure.

#include <vtbackend/graphics/KittyMediumSource.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;
using namespace vtbackend;
using namespace vtbackend::kitty_graphics;

namespace
{
/// The path Claude Code's probe names, as it writes it.
constexpr auto ProbePath = "/tmp/tty-graphics-protocol-probe.rgba"sv;

/// A medium source that serves fixed bytes, or a fixed refusal, and records what it was asked.
class FakeMediumSource final: public KittyMediumSource
{
  public:
    std::expected<std::vector<uint8_t>, MediaRefusal> result = std::vector<uint8_t> { 1, 2, 3, 255 };
    mutable std::vector<std::pair<std::string, Medium>> requests;

    [[nodiscard]] std::expected<std::vector<uint8_t>, MediaRefusal> read(
        std::string_view path, Medium medium, MediaPolicy const& /*policy*/) const override
    {
        requests.emplace_back(std::string { path }, medium);
        return result;
    }
};

[[nodiscard]] MediaPolicy enabledPolicy()
{
    return MediaPolicy { .transmission = FileTransmission::Enabled,
                         .temporaryRoots = { "/tmp" },
                         .temporaryFileOnlyRoots = {},
                         .allowedRoots = {},
                         .userId = 0,
                         .sizeLimit = 1024 };
}

/// A terminal that reads kitty file media from @p source.
[[nodiscard]] KittyMediumServices servicesOf(FakeMediumSource const& source)
{
    return KittyMediumServices { .source = source, .policy = enabledPolicy() };
}

/// @return Claude Code's 1x1 RGBA file probe, with the medium @p medium.
[[nodiscard]] std::string probe(char medium = 'f', std::string_view action = "q")
{
    return std::format(
        "\033_Ga={},i=31,s=1,v=1,t={},f=32;{}\033\\", action, medium, core::base64::encode(ProbePath));
}

/// Puts a local shell in the foreground of @p mock's PTY: a terminal that cannot tell what is in the
/// foreground reads no file.
void runsLocalShell(MockTerm<vtpty::MockPty>& mock)
{
    mock.mockPty().setForegroundProcessName("bash");
}

constexpr auto Ok = "\033_Gi=31;OK\033\\"sv;
constexpr auto Refused = "\033_Gi=31;EBADF:file not readable\033\\"sv;
} // namespace

TEST_CASE("KittyMedia.probe_from_the_pty_is_read", "[kitty][media]")
{
    auto const source = FakeMediumSource {};
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);

    mock.writeToScreen(probe());

    CHECK(mock.terminal.peekInput() == Ok);
    REQUIRE(source.requests.size() == 1);
    CHECK(source.requests[0].first == ProbePath);
    CHECK(source.requests[0].second == Medium::File);
    // A query stores nothing.
    CHECK(mock.terminal.primaryScreen().kittyImage(31) == nullptr);
}

TEST_CASE("KittyMedia.an_overlong_path_reaches_no_source", "[kitty][media]")
{
    auto const source = FakeMediumSource {};
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);
    auto const sent = [&](size_t length) {
        auto const path = "/tmp/" + std::string(length - 5, 'x');
        mock.discardPendingReplies();
        mock.writeToScreen(
            std::format("\033_Ga=q,i=31,s=1,v=1,t=f,f=32;{}\033\\", core::base64::encode(path)));
        return mock.terminal.peekInput();
    };

    CHECK(sent(MaxFilePathLength) == Ok);
    CHECK(sent(MaxFilePathLength + 1) == Refused);
    CHECK(source.requests.size() == 1);
}

TEST_CASE("KittyMedia.temporary_file_reaches_the_source_as_such", "[kitty][media]")
{
    auto const source = FakeMediumSource {};
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);

    mock.writeToScreen(probe('t'));

    CHECK(mock.terminal.peekInput() == Ok);
    REQUIRE(source.requests.size() == 1);
    CHECK(source.requests[0].second == Medium::TemporaryFile);
}

TEST_CASE("KittyMedia.transmission_stores_the_file", "[kitty][media]")
{
    auto const source = FakeMediumSource {};
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);

    mock.writeToScreen(probe('f', "t"));

    CHECK(mock.terminal.peekInput() == Ok);
    auto const image = mock.terminal.primaryScreen().kittyImage(31);
    REQUIRE(image != nullptr);
    CHECK(image->data() == std::vector<uint8_t> { 1, 2, 3, 255 });
}

TEST_CASE("KittyMedia.a_frame_reads_the_file", "[kitty][media]")
{
    auto const source = FakeMediumSource {};
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);
    mock.writeToScreen("\033_Ga=t,i=31,s=1,v=1,f=32;AAAA/w==\033\\"sv);
    mock.discardPendingReplies();

    mock.writeToScreen(probe('f', "f"));

    CHECK(mock.terminal.peekInput() == Ok);
    CHECK(source.requests.size() == 1);
    CHECK(mock.terminal.primaryScreen().kittyFrameImage(31, 2) != nullptr);
}

TEST_CASE("KittyMedia.every_failure_gets_one_refusal", "[kitty][media]")
{
    auto source = FakeMediumSource {};

    SECTION("the source refuses")
    {
        source.result = std::unexpected(MediaRefusal::OutsideRoots);
        auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
        runsLocalShell(mock);
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Refused);
    }

    SECTION("the file does not match the dimensions")
    {
        source.result = std::vector<uint8_t> { 1, 2, 3 };
        auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
        runsLocalShell(mock);
        mock.writeToScreen(probe('f', "t"));
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(mock.terminal.primaryScreen().kittyImage(31) == nullptr);
    }

    SECTION("t=s is never read")
    {
        auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
        runsLocalShell(mock);
        mock.writeToScreen(probe('s'));
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(source.requests.empty());
    }

    SECTION("a terminal without medium services")
    {
        auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Refused);
    }

    SECTION("a disabled policy")
    {
        auto services = servicesOf(source);
        services.policy.transmission = FileTransmission::Disabled;
        auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, services };
        runsLocalShell(mock);
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(source.requests.empty());
    }
}

TEST_CASE("KittyMedia.only_bytes_from_a_local_pty_are_read", "[kitty][media]")
{
    auto const source = FakeMediumSource {};
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);

    SECTION("written by the host, not the PTY")
    {
        mock.terminal.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(source.requests.empty());

        // The PTY's next batch is read again: the host's write left no origin behind.
        mock.discardPendingReplies();
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Ok);
    }

    SECTION("while the active context is behind a host boundary")
    {
        mock.writeToScreen("\033]3008;start=abc;type=container;container=foobar\033\\"sv);
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(source.requests.empty());
    }

    SECTION("on a replica of a daemon's terminal")
    {
        mock.terminal.bindReplica(ReplicaRole::SameMachine, HostIdentity {});
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(source.requests.empty());
    }
}

TEST_CASE("KittyMedia.not_while_a_remote_shell_client_is_in_the_foreground", "[kitty][media]")
{
    // `ssh host` typed into a local pane, with no shell integration to say so: the PTY's foreground process
    // tells that the bytes come from over there.
    auto const source = FakeMediumSource {};
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);

    SECTION("ssh")
    {
        mock.mockPty().setForegroundProcessName("ssh");
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(source.requests.empty());
    }

    SECTION("bash")
    {
        mock.mockPty().setForegroundProcessName("bash");
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Ok);
    }

    SECTION("none the PTY can name")
    {
        mock.mockPty().setForegroundProcessName(std::nullopt);
        mock.writeToScreen(probe());
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(source.requests.empty());
    }
}

TEST_CASE("KittyMedia.a_quota_refusal_after_a_read_is_the_one_refusal", "[kitty][media]")
{
    // Inside the roots, ENOSPC would tell the application more about a file than the uniform refusal does.
    auto const source = FakeMediumSource {};
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);
    mock.terminal.settings().kittyImageStorageQuota = 6;
    mock.writeToScreen("\033_Ga=t,i=1,s=1,v=1,f=32,q=2;AAAA/w==\033\\"sv);

    mock.writeToScreen(probe('f', "t"));

    CHECK(mock.terminal.peekInput() == Refused);
    CHECK(mock.terminal.primaryScreen().kittyImage(31) == nullptr);
}

namespace
{
/// A terminal that runs a test's code from the events a parse raises.
class EventProbeTerm final: public MockTerm<vtpty::MockPty>
{
  public:
    using MockTerm::MockTerm;

    std::function<void()> onScreenUpdated;
    std::function<void()> onBell;

    void screenUpdated() override
    {
        if (onScreenUpdated)
            onScreenUpdated();
    }

    void bell() override
    {
        if (onBell)
            onBell();
    }
};
} // namespace

TEST_CASE("KittyMedia.what_the_host_writes_mid_batch_is_not_the_ptys", "[kitty][media]")
{
    auto const source = FakeMediumSource {};
    auto mock = EventProbeTerm { PageSize { LineCount(4), ColumnCount(8) }, servicesOf(source) };
    runsLocalShell(mock);

    SECTION("a status line written while the batch is parsed")
    {
        mock.onBell = [&] {
            mock.terminal.writeToScreenInternal(mock.terminal.primaryScreen(), probe());
        };
        mock.writeToScreen("\a"sv);
        CHECK(mock.terminal.peekInput() == Refused);
        CHECK(source.requests.empty());
    }

    SECTION("anything while a smooth-scroll pause lets go of the lock")
    {
        mock.terminal.settings().smoothLineScrolling = std::chrono::milliseconds { 1 };
        auto mediaInPause = std::optional<bool> {};
        mock.onScreenUpdated = [&] {
            if (!mediaInPause)
                mediaInPause = mock.terminal.kittyMediaForCurrentBatch() != nullptr;
        };
        // The pause comes from the linefeed; the probe after it in the same batch is the PTY's again.
        mock.writeToScreen("\033[?4h\n" + probe());
        REQUIRE(mediaInPause.has_value());
        CHECK_FALSE(*mediaInPause);
        CHECK(mock.terminal.peekInput() == Ok);
    }
}
