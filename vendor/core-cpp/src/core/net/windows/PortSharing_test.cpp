// SPDX-License-Identifier: Apache-2.0
//
// `ListenOptions::sharing` on Windows: refused. POSIX's case, where it sets SO_REUSEPORT, is
// posix/PortSharing_test.cpp.

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/UdpSocket.hpp>

#include <catch2/catch_test_macros.hpp>

using core::net::EventLoop;

TEST_CASE("PortSharing::Shared is refused on Windows rather than mapped to SO_REUSEADDR", "[net][listen]")
{
    // Windows has no load-balancing SO_REUSEPORT. Its SO_REUSEADDR lets a later socket take over a
    // port another one holds, which is a hijack rather than a share, so the request is refused.
    auto backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };

    auto refused = core::net::listen(
        loop, core::net::ListenOptions { .host = "127.0.0.1", .sharing = core::net::PortSharing::Shared });
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == core::net::NetErrorCode::Unsupported);
}
