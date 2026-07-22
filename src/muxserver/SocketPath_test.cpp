// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <muxserver/SocketPath.h>

using muxserver::muxSocketPath;

TEST_CASE("an explicit --socket path wins over everything", "[muxserver][socketpath]")
{
    CHECK(muxSocketPath("default", "/tmp/explicit.sock", "/env/mux", "/run/user/1000")
          == "/tmp/explicit.sock");
}

TEST_CASE("$CONTOUR_MUX overrides the derived path", "[muxserver][socketpath]")
{
    CHECK(muxSocketPath("default", "", "/env/mux.sock", "/run/user/1000") == "/env/mux.sock");
}

TEST_CASE("the XDG runtime dir hosts the per-label socket", "[muxserver][socketpath]")
{
    CHECK(muxSocketPath("default", "", std::nullopt, "/run/user/1000") == "/run/user/1000/contour/default");
    CHECK(muxSocketPath("work", "", std::nullopt, "/run/user/1000") == "/run/user/1000/contour/work");
}

TEST_CASE("an empty env value counts as unset", "[muxserver][socketpath]")
{
    CHECK(muxSocketPath("default", "", "", "/run/user/1000") == "/run/user/1000/contour/default");
}

TEST_CASE("without a runtime dir the path falls back to a per-uid temp dir", "[muxserver][socketpath]")
{
    auto const path = muxSocketPath("fallback", "", std::nullopt, std::nullopt);
    CHECK(path.string().contains("contour-")); // the per-uid temp dir carries the prefix
    CHECK(path.filename() == "fallback");      // the label is the leaf, whatever the OS separator
}
