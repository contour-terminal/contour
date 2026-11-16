// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Clock.hpp>
#include <core/tui/ClipboardProtocol.hpp>
#include <core/tui/ClipboardWriter.hpp>
#include <core/tui/testing/ScriptedTerminalChannel.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using core::platform::ManualClock;
using core::tui::ChannelAccess;
using core::tui::ClipboardTarget;
using core::tui::ClipboardTransport;
using core::tui::ClipboardWriteError;
using core::tui::ClipboardWriter;
using core::tui::ClipboardWriterTimeouts;
using core::tui::encodeOsc52;
using core::tui::OscResponse;
using core::tui::testing::ScriptedTerminal;
using core::tui::testing::TerminalPresence;

using namespace std::chrono_literals;

namespace
{

constexpr auto Probe = core::tui::Osc5522Probe;

/// A terminal that answers the DECRQM probe with @p decModeStatus (nullopt: not at all) and an
/// OSC 5522 write with @p writeStatus (nullopt: not at all); DA1 is always answered.
auto terminalWith(std::optional<int> decModeStatus, std::optional<std::string> writeStatus = std::nullopt)
    -> ScriptedTerminal
{
    auto terminal = ScriptedTerminal {};
    terminal.decModeStatus = decModeStatus;
    terminal.writeStatus = std::move(writeStatus);
    return terminal;
}

/// A terminal that speaks OSC 5522 and confirms every write.
auto kittyLike() -> ScriptedTerminal
{
    return terminalWith(2, "DONE");
}

/// A terminal that answers nothing at all.
auto silentTerminal() -> ScriptedTerminal
{
    auto terminal = ScriptedTerminal {};
    terminal.answersDeviceAttributes = false;
    return terminal;
}

/// How far @p clock has moved since the epoch it started at.
auto elapsed(ManualClock const& clock)
{
    return clock.now() - core::platform::SteadyTimePoint {};
}

} // namespace

TEST_CASE("tui.ClipboardWriter: a terminal that knows mode 5522 gets the copy over OSC 5522")
{
    auto const status = GENERATE(1, 2, 3);
    CAPTURE(status);
    auto clock = ManualClock {};
    auto terminal = terminalWith(status, "DONE");
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE(result.has_value());
    CHECK(*result == ClipboardTransport::Osc5522);
    REQUIRE(terminal.written.size() == 2);
    CHECK(terminal.written[0] == Probe);
    CHECK(terminal.written[1].starts_with("\033]5522;type=write"));
    CHECK(terminal.decodedPayload() == "hi");
    CHECK(terminal.mimeType() == "text/plain");
}

TEST_CASE("tui.ClipboardWriter: a terminal that does not know mode 5522 gets OSC 52")
{
    auto const status = GENERATE(0, 4);
    CAPTURE(status);
    auto clock = ManualClock {};
    auto terminal = terminalWith(status);
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE(result.has_value());
    CHECK(*result == ClipboardTransport::Osc52);
    REQUIRE(terminal.written.size() == 2);
    CHECK(terminal.written[1] == encodeOsc52("hi", ClipboardTarget::Clipboard));
}

TEST_CASE("tui.ClipboardWriter: the DA1 reply ends the probe without waiting for a DECRQM reply")
{
    auto clock = ManualClock {};
    auto terminal = ScriptedTerminal {}; // answers DA1 only
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE(result.has_value());
    CHECK(*result == ClipboardTransport::Osc52);
    CHECK(elapsed(clock) < 500ms);
}

TEST_CASE("tui.ClipboardWriter: a terminal that answers nothing gets OSC 52 once the probe times out")
{
    auto clock = ManualClock {};
    auto terminal = silentTerminal();
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE(result.has_value());
    CHECK(*result == ClipboardTransport::Osc52);
    CHECK(elapsed(clock) >= 500ms);
    CHECK(terminal.decodedPayload() == "hi");
}

TEST_CASE("tui.ClipboardWriter: the probe runs once, and each copy opens its own channel")
{
    auto clock = ManualClock {};
    auto terminal = kittyLike();
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    REQUIRE(writer.write("one", "text/plain", ClipboardTarget::Clipboard).has_value());
    REQUIRE(writer.write("two", "text/plain", ClipboardTarget::Clipboard).has_value());

    CHECK(terminal.writesContaining(core::tui::Osc5522ModeQuery) == 1);
    CHECK(terminal.opens == 2);
    CHECK(terminal.decodedPayload() == "two");
}

TEST_CASE("tui.ClipboardWriter: a non-text MIME type is refused over OSC 52 before anything is sent")
{
    auto clock = ManualClock {};
    auto terminal = terminalWith(0);
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("\x89PNG", "image/png", ClipboardTarget::Clipboard);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ClipboardWriteError::UnsupportedMimeType);
    CHECK(terminal.written.size() == 1); // the probe only
}

TEST_CASE("tui.ClipboardWriter: a non-text MIME type travels over OSC 5522")
{
    auto clock = ManualClock {};
    auto terminal = kittyLike();
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("<b>x</b>", "text/html", ClipboardTarget::Primary);

    REQUIRE(result.has_value());
    CHECK(terminal.mimeType() == "text/html");
    CHECK(terminal.target() == ClipboardTarget::Primary);
}

TEST_CASE("tui.ClipboardWriter: an OSC 5522 error status is the copy's error")
{
    auto const [status, expected] = GENERATE(table<std::string, ClipboardWriteError>({
        { "EPERM", ClipboardWriteError::PermissionDenied },
        { "EFBIG", ClipboardWriteError::TooLarge },
        { "ENOSYS", ClipboardWriteError::PrimaryUnavailable },
    }));
    CAPTURE(status);
    auto clock = ManualClock {};
    auto terminal = terminalWith(2, status);
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == expected);
}

TEST_CASE("tui.ClipboardWriter: an OSC 5522 write nobody confirms fails after the status timeout")
{
    auto clock = ManualClock {};
    auto terminal = terminalWith(2); // no write status
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ClipboardWriteError::NoConfirmation);
    CHECK(elapsed(clock) >= 5000ms);
}

TEST_CASE("tui.ClipboardWriter: an unrelated OSC reply ahead of the status is ignored")
{
    auto clock = ManualClock {};
    auto terminal = kittyLike();
    terminal.beforeWriteStatus.emplace_back(OscResponse { .payload = "11;rgb:0000/0000/0000" });
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE(result.has_value());
    CHECK(*result == ClipboardTransport::Osc5522);
}

TEST_CASE("tui.ClipboardWriter: a write-only channel is not probed, and that is not remembered")
{
    auto clock = ManualClock {};
    auto terminal = kittyLike();
    terminal.access = ChannelAccess::WriteOnly;
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const background = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);
    REQUIRE(background.has_value());
    CHECK(*background == ClipboardTransport::Osc52);
    CHECK(terminal.writesContaining(core::tui::Osc5522ModeQuery) == 0);

    // Back in the foreground, the terminal is asked after all.
    terminal.access = ChannelAccess::ReadWrite;
    auto const foreground = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);
    REQUIRE(foreground.has_value());
    CHECK(*foreground == ClipboardTransport::Osc5522);
}

TEST_CASE("tui.ClipboardWriter: a reply left over from an earlier copy does not confirm this one")
{
    auto clock = ManualClock {};
    auto terminal = terminalWith(2); // knows OSC 5522, never confirms
    auto writer = ClipboardWriter { terminal.factory(clock), clock };
    REQUIRE(writer.write("probe", "text/plain", ClipboardTarget::Clipboard).error()
            == ClipboardWriteError::NoConfirmation);

    // The previous copy's status arrives late, and is waiting when the next copy opens its channel.
    terminal.queuedAtOpen.emplace_back(OscResponse { .payload = "5522;type=write:status=DONE" });
    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ClipboardWriteError::NoConfirmation);
}

TEST_CASE("tui.ClipboardWriter: an OSC 52 copy, which waits for nothing, keeps what was typed ahead")
{
    auto clock = ManualClock {};
    auto terminal = terminalWith(0);
    auto writer = ClipboardWriter { terminal.factory(clock), clock };
    REQUIRE(writer.write("one", "text/plain", ClipboardTarget::Clipboard).has_value()); // probes
    auto const discardsAfterProbe = terminal.discards;

    REQUIRE(writer.write("two", "text/plain", ClipboardTarget::Clipboard).has_value());
    CHECK(terminal.discards == discardsAfterProbe);
}

TEST_CASE("tui.ClipboardWriter: no terminal to open is NoTerminal")
{
    auto clock = ManualClock {};
    auto terminal = ScriptedTerminal {};
    terminal.presence = TerminalPresence::Absent;
    auto writer = ClipboardWriter { terminal.factory(clock), clock };

    auto const result = writer.write("hi", "text/plain", ClipboardTarget::Clipboard);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ClipboardWriteError::NoTerminal);
}

TEST_CASE("tui.ClipboardWriter: the timeouts are the writer's to choose")
{
    auto clock = ManualClock {};
    auto terminal = silentTerminal();
    auto writer = ClipboardWriter { terminal.factory(clock),
                                    clock,
                                    ClipboardWriterTimeouts { .probe = 50ms, .status = 5000ms } };

    REQUIRE(writer.write("hi", "text/plain", ClipboardTarget::Clipboard).has_value());
    CHECK(elapsed(clock) >= 50ms);
    CHECK(elapsed(clock) < 500ms);
}
