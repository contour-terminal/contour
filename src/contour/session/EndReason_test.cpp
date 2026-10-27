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
    CHECK(paneFooterMessage(EndReason::Exited, "1") == "Process exited with code 1.");
    CHECK(paneFooterMessage(EndReason::Exited, "") == "Process exited.");
    CHECK(paneFooterMessage(EndReason::Signalled, "9") == "Process ended by signal 9.");
    CHECK(paneFooterMessage(EndReason::Signalled, "") == "Process ended.");
    CHECK(paneFooterMessage(EndReason::StartFailed, "no such directory")
          == "The terminal could not be started: no such directory.");
    CHECK(paneFooterMessage(EndReason::StartFailed, "") == "The terminal could not be started.");
    CHECK(paneFooterMessage(EndReason::Disconnected, "")
          == "Disconnected from the Contour daemon. Its sessions are still running; reattach with: contour "
             "client");
    CHECK(paneFooterMessage(EndReason::None, "").empty());
}

TEST_CASE("paneFooter.restartIsOfferedOnlyWhereTheRowAndTheHostingAllowIt", "[panestate]")
{
    CHECK(offeredAction(EndReason::Exited, RestartSupport::Supported) == FooterAction::Restart);
    CHECK(offeredAction(EndReason::Signalled, RestartSupport::Supported) == FooterAction::Restart);
    CHECK(offeredAction(EndReason::StartFailed, RestartSupport::Supported) == FooterAction::Restart);
    CHECK(offeredAction(EndReason::Exited, RestartSupport::Unsupported) == FooterAction::None);
    // Disconnected offers Close only: no reconnect exists to put behind the button.
    CHECK(offeredAction(EndReason::Disconnected, RestartSupport::Supported) == FooterAction::None);
    CHECK(offeredAction(EndReason::None, RestartSupport::Supported) == FooterAction::None);
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
