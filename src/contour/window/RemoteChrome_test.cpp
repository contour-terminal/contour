// SPDX-License-Identifier: Apache-2.0
#include <contour/window/RemoteChrome.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::window::remoteWindowTitle;

TEST_CASE("a remote window's title names its host", "[contour][hosts]")
{
    CHECK(remoteWindowTitle("home", "vim") == "home — vim");
    CHECK(remoteWindowTitle("ssh://me@homepc:2222", "zsh") == "ssh://me@homepc:2222 — zsh");
}

TEST_CASE("a local window's title is its session's alone", "[contour][hosts]")
{
    CHECK(remoteWindowTitle("", "vim") == "vim");
    CHECK(remoteWindowTitle("", "").empty());
}

TEST_CASE("a remote window whose session has no title yet shows the host", "[contour][hosts]")
{
    CHECK(remoteWindowTitle("home", "") == "home");
}
