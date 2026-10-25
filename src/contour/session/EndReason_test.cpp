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
          == "Disconnected from the Contour daemon. Its sessions are still running.");
    CHECK(paneFooterMessage(EndReason::None, "").empty());
}

TEST_CASE("paneFooter.restartIsOfferedOnlyWhereTheRowAndTheHostingAllowIt", "[panestate]")
{
    CHECK(restartOf(EndReason::Exited, RestartSupport::Supported) == FooterRestart::Offered);
    CHECK(restartOf(EndReason::Signalled, RestartSupport::Supported) == FooterRestart::Offered);
    CHECK(restartOf(EndReason::StartFailed, RestartSupport::Supported) == FooterRestart::Offered);
    CHECK(restartOf(EndReason::Exited, RestartSupport::Unsupported) == FooterRestart::Unavailable);
    // Disconnected offers Close only: no reconnect exists to put behind the button.
    CHECK(restartOf(EndReason::Disconnected, RestartSupport::Supported) == FooterRestart::Unavailable);
    CHECK(restartOf(EndReason::None, RestartSupport::Supported) == FooterRestart::Unavailable);
}
