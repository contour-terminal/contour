// SPDX-License-Identifier: Apache-2.0

#include <contour/platform/FinishNotification.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/PaneState.hpp>

#include <vtbackend/shell/CommandLineText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

#include <vthost/client/ConnectionState.hpp>

using namespace contour::window;
using contour::session::EndReason;
using vthost::client::ConnectionState;

namespace
{
constexpr auto AllEnds = std::array {
    EndReason::None, EndReason::Exited, EndReason::Signalled, EndReason::StartFailed, EndReason::Disconnected,
};
constexpr auto AllConnections = std::array {
    ConnectionState::Connecting, ConnectionState::Connected, ConnectionState::Reconnecting,
    ConnectionState::Failed,     ConnectionState::Refused,   ConnectionState::Stopped,
};
} // namespace

TEST_CASE("paneState.followsTheEndThenTheConnection", "[panestate]")
{
    for (auto const end: AllEnds)
        for (auto const connection: AllConnections)
        {
            auto const expected = [&] {
                if (end != EndReason::None)
                    return PaneState::Ended;
                // A first connection reads as reconnecting, as the window's host chip says.
                if (connection == ConnectionState::Connecting || connection == ConnectionState::Reconnecting)
                    return PaneState::Reconnecting;
                return PaneState::Live;
            }();
            INFO("end " << static_cast<int>(end) << ", connection " << static_cast<int>(connection));
            CHECK(paneStateOf(end, connection) == expected);
        }
}

TEST_CASE("paneState.anEndedPaneIsEndedEvenWhileReconnecting", "[panestate]")
{
    CHECK(paneStateOf(EndReason::Disconnected, ConnectionState::Reconnecting) == PaneState::Ended);
    CHECK(paneStateOf(EndReason::None, ConnectionState::Reconnecting) == PaneState::Reconnecting);
    CHECK(paneStateOf(EndReason::None, ConnectionState::Connected) == PaneState::Live);
}

TEST_CASE("paneState.foldsTheConnectionAsTheHostChipDoes", "[panestate]")
{
    // The pane's pill and the window's host chip never disagree about a connection being sought.
    for (auto const connection: AllConnections)
    {
        INFO("connection " << static_cast<int>(connection));
        CHECK((paneStateOf(EndReason::None, connection) == PaneState::Reconnecting)
              == (hostChipStateOf(connection) == HostChipState::Reconnecting));
    }
}

TEST_CASE("paneFooter.anEndWinsOverAnInterruption", "[panestate]")
{
    for (auto const end: AllEnds)
        for (auto const interruption: { InterruptionPresence::Absent, InterruptionPresence::Present })
        {
            auto const expected = [&] {
                if (end != EndReason::None)
                    return PaneFooterKind::Ended;
                return interruption == InterruptionPresence::Present ? PaneFooterKind::Interrupted
                                                                     : PaneFooterKind::None;
            }();
            INFO("end " << static_cast<int>(end) << ", interruption " << static_cast<int>(interruption));
            CHECK(paneFooterOf(end, interruption) == expected);
        }
}

TEST_CASE("paneFooter.runAgainWaitsForAPrompt", "[panestate]")
{
    CHECK(interruptionActionOf(ShellPromptPresence::Absent) == contour::session::FooterAction::None);
    CHECK(interruptionActionOf(ShellPromptPresence::Present) == contour::session::FooterAction::RunAgain);
}

TEST_CASE("paneFooter.interruptedMessageNamesTheCommand", "[panestate]")
{
    using vtbackend::CommandLineText;
    CHECK(interruptedFooterMessage(CommandLineText { "make test" })
          == "\u201cmake test\u201d was stopped when the session ended.");
}

TEST_CASE("paneFooter.interruptedMessageCutsALongCommandAfterSanitizing", "[panestate]")
{
    using vtbackend::CommandLineText;
    auto const prefix = std::string { "\u201c" };
    auto const suffix = std::string { "\u201d was stopped when the session ended." };
    auto const commandOf = [&](std::string const& message) {
        REQUIRE(message.starts_with(prefix));
        REQUIRE(message.ends_with(suffix));
        return message.substr(prefix.size(), message.size() - prefix.size() - suffix.size());
    };

    // A long line is cut, its ellipsis counted in the 48 bytes.
    auto const longCommand = commandOf(interruptedFooterMessage(CommandLineText { std::string(200, 'x') }));
    CHECK(longCommand.size() <= contour::platform::MaxCommandNameBytes);
    CHECK(longCommand.ends_with("\u2026"));

    // Controls and a bidi override become replacement characters, each longer than the byte it replaces:
    // the cut is made on what is shown, so the command is still within the bound.
    // Built byte by byte (U+202E, RIGHT-TO-LEFT OVERRIDE), so no source literal holds a misleading character.
    auto const bidiOverride = std::string { '\xE2', '\x80', '\xAE' };
    auto const hostile = "echo " + bidiOverride + "evil; rm -rf ~\n" + std::string(100, '\x01');
    auto const shown = commandOf(interruptedFooterMessage(CommandLineText { hostile }));
    CHECK(shown.size() <= contour::platform::MaxCommandNameBytes);
    CHECK_FALSE(shown.contains(bidiOverride));
    CHECK_FALSE(shown.contains('\n'));
    CHECK_FALSE(shown.contains('\x01'));
}
