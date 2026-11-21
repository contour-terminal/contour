// SPDX-License-Identifier: Apache-2.0
//
// Which sessions may have kitty graphics read local files for them: those whose bytes come from a local
// process, and no other.

#include <contour/session/MediaAccess.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using contour::session::MediaAccess;
using contour::session::mediaAccessFor;
using contour::session::SessionTransport;

TEST_CASE("mediaAccessFor: only a local process reads local files", "[contour][media]")
{
    struct Row
    {
        SessionTransport transport;
        MediaAccess access;
    };
    auto const row = GENERATE(Row { SessionTransport::Unknown, MediaAccess::None },
                              Row { SessionTransport::LocalProcess, MediaAccess::LocalFiles },
                              Row { SessionTransport::Ssh, MediaAccess::None },
                              Row { SessionTransport::TmuxPane, MediaAccess::None },
                              Row { SessionTransport::DaemonReplica, MediaAccess::None });
    CHECK(mediaAccessFor(row.transport) == row.access);
    STATIC_CHECK(mediaAccessFor(SessionTransport::LocalProcess) == MediaAccess::LocalFiles);
}
