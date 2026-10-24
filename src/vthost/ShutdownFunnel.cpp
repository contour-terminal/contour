// SPDX-License-Identifier: Apache-2.0
#include <vthost/ShutdownFunnel.hpp>

#include <core/net/EventLoop.hpp>

#include <utility>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/Logging.hpp>

namespace vthost
{

ShutdownFunnel::ShutdownFunnel(core::net::EventLoop& loop,
                               std::vector<ConnectionAcceptor*> const& servers,
                               std::function<void(ShutdownCause)> beforeShutdown):
    _loop(loop), _servers(servers), _beforeShutdown(std::move(beforeShutdown))
{
}

void ShutdownFunnel::request(ShutdownCause cause)
{
    if (_state == ShutdownState::ShuttingDown)
        return;
    _state = ShutdownState::ShuttingDown;

    daemonLog()("{}; closing {} listener(s)", nameOf(cause), _servers.size());
    // First, while every listener still accepts and every session is alive: that is the hook's contract.
    if (_beforeShutdown)
        _beforeShutdown(cause);
    for (auto* each: _servers)
        each->close();
    _loop.requestStop();
}

} // namespace vthost
