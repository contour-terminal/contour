// SPDX-License-Identifier: Apache-2.0

#include <contour/session/EndReason.hpp>
#include <contour/window/PaneState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

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
                if (connection == ConnectionState::Reconnecting)
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
