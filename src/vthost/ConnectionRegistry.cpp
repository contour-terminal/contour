// SPDX-License-Identifier: Apache-2.0
#include <vthost/ConnectionRegistry.hpp>

#include <core/net/ISocket.hpp>

#include <utility>
#include <vector>

namespace vthost
{

namespace
{
    /// One tracked connection's place in the registry, for as long as it lives. Move-only; a moved-from one
    /// holds nothing.
    class Registration
    {
      public:
        Registration(std::shared_ptr<std::unordered_set<core::net::ISocket*>> sockets,
                     core::net::ISocket* socket):
            _sockets(std::move(sockets)), _socket(socket)
        {
            _sockets->insert(_socket);
        }
        Registration(Registration const&) = delete;
        Registration& operator=(Registration const&) = delete;
        Registration(Registration&& other) noexcept:
            _sockets(std::move(other._sockets)), _socket(std::exchange(other._socket, nullptr))
        {
        }
        Registration& operator=(Registration&&) = delete;
        ~Registration()
        {
            if (_sockets)
                _sockets->erase(_socket);
        }

      private:
        std::shared_ptr<std::unordered_set<core::net::ISocket*>> _sockets;
        core::net::ISocket* _socket;
    };

    /// One tracked connection, as a free coroutine: @p registration is held by its frame, which a
    /// coroutine's parameters live in, for as long as @p connection runs.
    core::async::Task<void> runTracked([[maybe_unused]] Registration registration,
                                       core::async::Task<void> connection)
    {
        co_await std::move(connection);
    }
} // namespace

ConnectionHandler ConnectionRegistry::tracked(ConnectionHandler handler) const
{
    // Aliases the set inside the shared state, which keeps the state alive.
    auto sockets = std::shared_ptr<std::unordered_set<core::net::ISocket*>>(_state, &_state->sockets);
    // NOT a coroutine itself: the registration is made as the connection is accepted, and moved into the
    // coroutine's frame.
    return [sockets = std::move(sockets),
            handler = std::move(handler)](ConnectionId id, std::unique_ptr<core::net::ISocket> connection) {
        auto registration = Registration { sockets, connection.get() };
        return runTracked(std::move(registration), handler(std::move(id), std::move(connection)));
    };
}

void ConnectionRegistry::closeAll() const
{
    // A copy: closing never unwinds a flow inline, but nothing here should depend on that.
    for (auto* const socket: std::vector<core::net::ISocket*>(_state->sockets.begin(), _state->sockets.end()))
        socket->close();
}

} // namespace vthost
