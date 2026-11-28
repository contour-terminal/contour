// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ScriptedConnector` -- an @c net::IConnector that dials nothing: each connection is a fresh
/// in-memory socket pair, whose client end the dial answers and whose server end a test accepts.

#include <core/async/Task.hpp>
#include <core/http/HttpExport.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace core::http::testing
{

/// One dial a @c ScriptedConnector was asked for, refused ones included.
struct ScriptedDial
{
    std::string host;         ///< The host, as the dial named it.
    std::uint16_t port = 0;   ///< The port.
    net::DialOptions options; ///< The budget and the options it asked for.
};

/// A connector whose connections are in-memory pairs (@c net::testing::InMemorySocketPair).
///
/// A dial answers the client end at once, without suspending, and queues the server end for
/// @c acceptNext; a dial after @c refuseNext answers that error instead and queues nothing. It never
/// suspends, so a dial's deadline and its stop token have nothing to cut short. Loop-affine like the
/// sockets it hands out: one thread, the loop's.
class CORE_HTTP_API ScriptedConnector final: public net::IConnector
{
  public:
    /// @param maxBytesInFlight What each direction of every pair holds before a writer waits; zero
    ///        for no bound (@c net::testing::InMemorySocketPair::create).
    explicit ScriptedConnector(std::size_t maxBytesInFlight = 0);
    ~ScriptedConnector() override;
    ScriptedConnector(ScriptedConnector const&) = delete;
    ScriptedConnector& operator=(ScriptedConnector const&) = delete;
    ScriptedConnector(ScriptedConnector&&) = delete;
    ScriptedConnector& operator=(ScriptedConnector&&) = delete;

    /// @copydoc net::IConnector::connect
    [[nodiscard]] async::Task<net::SocketResult> connect(std::string host,
                                                         std::uint16_t port,
                                                         net::DialOptions options) override;

    /// Answers the next dial with @p error. Queued: each call refuses one more dial.
    /// @param error What that dial answers.
    void refuseNext(net::NetError error);

    /// @return The server end of the oldest connection not yet accepted, or null when every one has
    ///         been.
    [[nodiscard]] std::unique_ptr<net::ISocket> acceptNext();

    /// @return How many dials were asked for, refused ones included.
    [[nodiscard]] std::size_t connects() const noexcept;

    /// @return Every dial asked for, in order, refused ones included.
    [[nodiscard]] std::span<ScriptedDial const> dials() const noexcept;

  private:
    std::size_t _maxBytesInFlight;
    std::deque<net::NetError> _refusals;
    std::deque<std::unique_ptr<net::ISocket>> _accepted;
    std::vector<ScriptedDial> _dials;
};

} // namespace core::http::testing
