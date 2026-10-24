// SPDX-License-Identifier: Apache-2.0
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include <vthost/StatePath.hpp>

namespace fs = std::filesystem;

using vthost::HostPlatform;

TEST_CASE("the daemon state directory follows each platform's convention", "[vthost][state]")
{
    CHECK(vthost::daemonStateDirectory({ .platform = HostPlatform::Linux, .home = "/home/u" })
          == fs::path { "/home/u/.local/state/contour" });
    CHECK(vthost::daemonStateDirectory(
              { .platform = HostPlatform::Linux, .xdgStateHome = "/x", .home = "/home/u" })
          == fs::path { "/x/contour" });
    CHECK(vthost::daemonStateDirectory(
              { .platform = HostPlatform::Linux, .xdgStateHome = "relative", .home = "/home/u" })
          == fs::path { "/home/u/.local/state/contour" });
    CHECK(vthost::daemonStateDirectory({ .platform = HostPlatform::MacOS, .home = "/Users/u" })
          == fs::path { "/Users/u/.local/state/contour" });
    // Written with '/', so the case decomposes the same on a POSIX host as on Windows.
    CHECK(vthost::daemonStateDirectory(
              { .platform = HostPlatform::Windows, .localAppData = "C:/Users/u/AppData/Local" })
          == fs::path { "C:/Users/u/AppData/Local" } / "contour");
#ifdef _WIN32
    CHECK(vthost::daemonStateDirectory(
              { .platform = HostPlatform::Windows, .localAppData = R"(C:\Users\u\AppData\Local)" })
          == fs::path { R"(C:\Users\u\AppData\Local)" } / "contour");
#endif
}

TEST_CASE("XDG_STATE_HOME applies on every POSIX system and nowhere else", "[vthost][state]")
{
    CHECK(vthost::daemonStateDirectory(
              { .platform = HostPlatform::MacOS, .xdgStateHome = "/x", .home = "/Users/u" })
          == fs::path { "/x/contour" });
    CHECK(vthost::daemonStateDirectory({ .platform = HostPlatform::OtherPosix, .xdgStateHome = "/x" })
          == fs::path { "/x/contour" });
    CHECK(vthost::daemonStateDirectory(
              { .platform = HostPlatform::Linux, .xdgStateHome = "", .home = "/home/u" })
          == fs::path { "/home/u/.local/state/contour" });
    CHECK(vthost::daemonStateDirectory(
              { .platform = HostPlatform::Windows, .xdgStateHome = "/x", .localAppData = "D:/Local" })
          == fs::path { "D:/Local" } / "contour");
}

TEST_CASE("absoluteness is judged by the target platform, not the host", "[vthost][state]")
{
    CHECK(vthost::isAbsoluteOn(HostPlatform::Linux, "/x"));
    CHECK_FALSE(vthost::isAbsoluteOn(HostPlatform::Linux, "C:/x"));
    CHECK_FALSE(vthost::isAbsoluteOn(HostPlatform::Windows, "/x"));
    CHECK(vthost::isAbsoluteOn(HostPlatform::Windows, R"(C:\x)"));
    CHECK(vthost::isAbsoluteOn(HostPlatform::Windows, "c:/x"));
    CHECK(vthost::isAbsoluteOn(HostPlatform::Windows, R"(\\server\share)"));
    CHECK_FALSE(vthost::isAbsoluteOn(HostPlatform::Windows, "C:x"));
    CHECK_FALSE(vthost::isAbsoluteOn(HostPlatform::Windows, "relative"));
}

TEST_CASE("without a usable directory the state lives in a per-user temp directory", "[vthost][state]")
{
    // Per user: the temp directory is shared, and one user's state must never be another's.
    auto const fallback = fs::temp_directory_path() / "contour-state-1000";
    CHECK(vthost::daemonStateDirectory({ .platform = HostPlatform::Linux, .user = "1000" }) == fallback);
    CHECK(
        vthost::daemonStateDirectory({ .platform = HostPlatform::Linux, .home = "relative", .user = "1000" })
        == fallback);
    CHECK(
        vthost::daemonStateDirectory({ .platform = HostPlatform::Windows, .home = "/home/u", .user = "1000" })
        == fallback);
    CHECK(vthost::daemonStateDirectory(
              { .platform = HostPlatform::Windows, .localAppData = "Local", .user = "1000" })
          == fallback);
    CHECK(vthost::daemonStateDirectory({ .platform = HostPlatform::Linux, .user = "1001" })
          == fs::temp_directory_path() / "contour-state-1001");
}

TEST_CASE("a fallback state directory is told apart from a configured one", "[vthost][state]")
{
    // Other local users can pre-create the fallback; what consumes the state directory must know.
    using vthost::StateDirectoryOrigin;
    CHECK(vthost::stateDirectoryOrigin({ .platform = HostPlatform::Linux, .home = "/home/u" })
          == StateDirectoryOrigin::Configured);
    CHECK(vthost::stateDirectoryOrigin({ .platform = HostPlatform::Linux, .xdgStateHome = "/x" })
          == StateDirectoryOrigin::Configured);
    CHECK(vthost::stateDirectoryOrigin({ .platform = HostPlatform::Windows, .localAppData = "C:/Local" })
          == StateDirectoryOrigin::Configured);
    CHECK(vthost::stateDirectoryOrigin({ .platform = HostPlatform::Linux, .user = "1000" })
          == StateDirectoryOrigin::TemporaryFallback);
    CHECK(vthost::stateDirectoryOrigin(
              { .platform = HostPlatform::Linux, .xdgStateHome = "rel", .home = "rel" })
          == StateDirectoryOrigin::TemporaryFallback);
    CHECK(vthost::stateDirectoryOrigin({ .platform = HostPlatform::Windows, .home = "/home/u" })
          == StateDirectoryOrigin::TemporaryFallback);
    CHECK(vthost::stateDirectoryOrigin(core::testing::FakeEnvironment {})
          == StateDirectoryOrigin::TemporaryFallback);
}

TEST_CASE("the production state directory reads the environment it is given", "[vthost][state]")
{
    auto const env = core::testing::FakeEnvironment {
        { { "XDG_STATE_HOME", "/xdg" }, { "LOCALAPPDATA", "C:/Local" }, { "HOME", "/home/u" } }
    };
    auto const expected = vthost::NativeHostPlatform == HostPlatform::Windows
                              ? fs::path { "C:/Local" } / "contour"
                              : fs::path { "/xdg/contour" };
    CHECK(vthost::daemonStateDirectory(env) == expected);
    CHECK(vthost::stateDirectoryOrigin(env) == vthost::StateDirectoryOrigin::Configured);
}
