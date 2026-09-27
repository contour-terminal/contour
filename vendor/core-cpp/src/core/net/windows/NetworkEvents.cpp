// SPDX-License-Identifier: Apache-2.0
#include <core/net/windows/NetworkEvents.hpp>

#include <core/net/windows/InvalidSocket.hpp>

namespace core::net
{

long consumeNetworkEvents(SOCKET socket, WSAEVENT event) noexcept
{
    if (socket == detail::InvalidSocket || event == WSA_INVALID_EVENT)
        return 0;
    auto events = WSANETWORKEVENTS {};
    if (WSAEnumNetworkEvents(socket, event, &events) != 0)
        return 0;
    return events.lNetworkEvents;
}

} // namespace core::net
