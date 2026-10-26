// SPDX-License-Identifier: Apache-2.0
#include <contour/window/HostChipState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>

using namespace contour::window;
using contour::config::StatusColorRole;
using vthost::client::ConnectionState;

TEST_CASE("hostChip.everyConnectionStateHasARow", "[hostchip]")
{
    REQUIRE(HostChipRows.size() == vthost::client::ConnectionStateCount);
    for (auto const i: std::views::iota(size_t { 0 }, HostChipRows.size()))
        CHECK(HostChipRows[i].connection == ConnectionState(i));
}

TEST_CASE("hostChip.stateTable", "[hostchip]")
{
    CHECK(hostChipStateOf(ConnectionState::Connecting) == HostChipState::Reconnecting);
    CHECK(hostChipStateOf(ConnectionState::Connected) == HostChipState::Connected);
    CHECK(hostChipStateOf(ConnectionState::Reconnecting) == HostChipState::Reconnecting);
    CHECK(hostChipStateOf(ConnectionState::Failed) == HostChipState::Disconnected);
    CHECK(hostChipStateOf(ConnectionState::Refused) == HostChipState::Disconnected);
    CHECK(hostChipStateOf(ConnectionState::Stopped) == HostChipState::Disconnected);
}

TEST_CASE("hostChip.reconnectRoundTrip", "[hostchip]")
{
    CHECK(hostChipStateOf(ConnectionState::Connected) == HostChipState::Connected);
    CHECK(hostChipStateOf(ConnectionState::Reconnecting) == HostChipState::Reconnecting);
    CHECK(hostChipStateOf(ConnectionState::Connected) == HostChipState::Connected);
    CHECK(hostChipStateOf(ConnectionState::Refused) == HostChipState::Disconnected);
}

TEST_CASE("hostChip.looks", "[hostchip]")
{
    for (auto const i: std::views::iota(size_t { 0 }, HostChipStateCount))
    {
        CHECK(HostChipLooks[i].state == HostChipState(i));
        CHECK_FALSE(HostChipLooks[i].accessibleState.empty());
    }
    CHECK(hostChipLook(HostChipState::Reconnecting).dot == StatusColorRole::Warning);
    CHECK(hostChipLook(HostChipState::Reconnecting).status == "Reconnecting…");
    CHECK(hostChipLook(HostChipState::Reconnecting).accessibleState == "reconnecting");
    CHECK(hostChipLook(HostChipState::Connected).status.empty());
    CHECK(hostChipLook(HostChipState::Connected).dot == StatusColorRole::Success);
    CHECK(hostChipLook(HostChipState::Connected).accessibleState == "connected");
    CHECK(hostChipLook(HostChipState::Disconnected).dot == StatusColorRole::Danger);
    CHECK(hostChipLook(HostChipState::Disconnected).status == "Disconnected");
    CHECK(hostChipLook(HostChipState::Disconnected).accessibleState == "disconnected");
}

TEST_CASE("hostChip.presence", "[hostchip]")
{
    CHECK(hostChipPresence("") == HostChipPresence::Hidden);
    CHECK(hostChipPresence("build-01") == HostChipPresence::Shown);
}

TEST_CASE("hostChip.notice", "[hostchip]")
{
    // Only a remote window whose connection is not up has news the band must carry.
    for (auto const i: std::views::iota(size_t { 0 }, HostChipStateCount))
        CHECK(hostNoticeOf(HostChipPresence::Hidden, HostChipState(i)) == HostNotice::None);
    CHECK(hostNoticeOf(HostChipPresence::Shown, HostChipState::Connected) == HostNotice::None);
    CHECK(hostNoticeOf(HostChipPresence::Shown, HostChipState::Reconnecting) == HostNotice::Interrupted);
    CHECK(hostNoticeOf(HostChipPresence::Shown, HostChipState::Disconnected) == HostNotice::Interrupted);
}
