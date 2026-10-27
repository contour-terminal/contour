// SPDX-License-Identifier: Apache-2.0

#include <contour/session/EndReason.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>

using namespace contour::session;

TEST_CASE("paneFooter.rowsAreInEnumeratorOrder", "[panestate]")
{
    for (auto const index: std::views::iota(std::size_t { 0 }, PaneFooterRows.size()))
        CHECK(static_cast<std::size_t>(PaneFooterRows.at(index).end) == index);
    CHECK(PaneFooterRows.back().end == EndReason::Disconnected);
}

TEST_CASE("paneFooter.messageNamesTheDetailWhenThereIsOne", "[panestate]")
{
    CHECK(paneFooterMessage(EndReason::Exited, "1", FooterAction::None) == "Process exited with code 1.");
    CHECK(paneFooterMessage(EndReason::Exited, "", FooterAction::None) == "Process exited.");
    CHECK(paneFooterMessage(EndReason::Signalled, "9", FooterAction::None) == "Process ended by signal 9.");
    CHECK(paneFooterMessage(EndReason::Signalled, "", FooterAction::None) == "Process ended.");
    CHECK(paneFooterMessage(EndReason::StartFailed, "no such directory", FooterAction::None)
          == "The terminal could not be started: no such directory.");
    CHECK(paneFooterMessage(EndReason::StartFailed, "", FooterAction::None)
          == "The terminal could not be started.");
    CHECK(paneFooterMessage(EndReason::Disconnected, "", FooterAction::None)
          == "Disconnected from the Contour daemon. Its sessions are still running; reattach with: contour "
             "client");
    CHECK(paneFooterMessage(EndReason::None, "", FooterAction::None).empty());
}

TEST_CASE("paneFooter.theActionIsOfferedOnlyWhereTheRowAndTheHostingAllowIt", "[panestate]")
{
    constexpr auto Local = RestartSupport::Supported;
    constexpr auto Remote = RestartSupport::Unsupported;
    constexpr auto Attached = ReconnectSupport::Supported;
    constexpr auto Detached = ReconnectSupport::Unsupported;

    CHECK(offeredAction(EndReason::Exited, Local, Detached) == FooterAction::Restart);
    CHECK(offeredAction(EndReason::Signalled, Local, Detached) == FooterAction::Restart);
    CHECK(offeredAction(EndReason::StartFailed, Local, Detached) == FooterAction::Restart);
    CHECK(offeredAction(EndReason::Exited, Remote, Attached) == FooterAction::None);
    // Reconnect needs an app attached to a daemon, whatever else the hosting does.
    CHECK(offeredAction(EndReason::Disconnected, Remote, Attached) == FooterAction::Reconnect);
    CHECK(offeredAction(EndReason::Disconnected, Local, Attached) == FooterAction::Reconnect);
    CHECK(offeredAction(EndReason::Disconnected, Remote, Detached) == FooterAction::None);
    CHECK(offeredAction(EndReason::Exited, Local, Attached) == FooterAction::Restart);
    CHECK(offeredAction(EndReason::None, Local, Attached) == FooterAction::None);
}

TEST_CASE("paneFooter.disconnectedDropsTheReattachHintWhileReconnectIsOffered", "[panestate]")
{
    constexpr auto HostWords = "Disconnected from the Contour daemon. Its sessions are still running.";
    CHECK(paneFooterMessage(EndReason::Disconnected, "", FooterAction::Reconnect) == HostWords);
    CHECK(paneFooterMessage(EndReason::Disconnected, "", FooterAction::None)
          == "Disconnected from the Contour daemon. Its sessions are still running; reattach with: contour "
             "client");
    // A refused reconnect says why, whichever button is offered.
    CHECK(paneFooterMessage(EndReason::Disconnected, "the daemon refused", FooterAction::Reconnect)
          == "Could not reconnect: the daemon refused.");
    // Other ends have no words for an offered action: Restart changes nothing of them.
    CHECK(paneFooterMessage(EndReason::Exited, "", FooterAction::Restart) == "Process exited.");
}

TEST_CASE("paneFooter.everyRowNamesAnActionLabelExactlyWhenItHasAnAction", "[panestate]")
{
    for (auto const& row: PaneFooterRows)
    {
        INFO("end " << static_cast<int>(row.end));
        CHECK(row.actionLabel.empty() == (row.action == FooterAction::None));
        // Only the interruption's footer runs a command again, and only an ended pane restarts.
        CHECK(row.action != FooterAction::RunAgain);
        CHECK(row.dismiss == FooterDismiss::Close);
    }
    CHECK(InterruptedFooterRow.action == FooterAction::RunAgain);
    CHECK_FALSE(InterruptedFooterRow.actionLabel.empty());
    CHECK(InterruptedFooterRow.dismiss == FooterDismiss::Dismiss);
}

TEST_CASE("paneFooter.dismissLabelsAreWordsWithoutTheirChord", "[panestate]")
{
    CHECK(dismissLabelOf(FooterDismiss::Close) == "Close");
    CHECK(dismissLabelOf(FooterDismiss::Dismiss) == "Dismiss");
}
