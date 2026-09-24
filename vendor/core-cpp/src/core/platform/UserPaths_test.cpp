// SPDX-License-Identifier: Apache-2.0
#include <core/platform/UserPaths.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using core::platform::configHome;
using core::platform::homeDirectory;
using core::platform::userName;
using core::testing::FakeEnvironment;

TEST_CASE("homeDirectory.returns_value_when_HOME_set", "[platform]")
{
    auto const environment =
        FakeEnvironment { { { "HOME", "/tmp/test_home" }, { "USERPROFILE", "/tmp/profile" } } };
    auto const home = homeDirectory(environment);
    REQUIRE(home.has_value());
    CHECK(*home == std::filesystem::path("/tmp/test_home"));
}

TEST_CASE("homeDirectory.falls_back_to_USERPROFILE", "[platform]")
{
    auto const environment = FakeEnvironment { { { "USERPROFILE", "/tmp/test_profile" } } };
    auto const home = homeDirectory(environment);
    REQUIRE(home.has_value());
    CHECK(*home == std::filesystem::path("/tmp/test_profile"));
}

TEST_CASE("homeDirectory.neither_variable_set", "[platform]")
{
    CHECK(!homeDirectory(FakeEnvironment {}).has_value());
}

TEST_CASE("configHome.returns_XDG_CONFIG_HOME_when_set", "[platform]")
{
    auto const environment = FakeEnvironment { { { "XDG_CONFIG_HOME", "/tmp/test_xdg_config" },
                                                 { "APPDATA", "/tmp/appdata" },
                                                 { "HOME", "/tmp/test_home" } } };
    auto const config = configHome(environment);
    REQUIRE(config.has_value());
    CHECK(*config == std::filesystem::path("/tmp/test_xdg_config"));
}

TEST_CASE("configHome.prefers_APPDATA_over_home", "[platform]")
{
    // An empty XDG_CONFIG_HOME counts as unset, as the XDG specification says.
    auto const environment = FakeEnvironment {
        { { "XDG_CONFIG_HOME", "" }, { "APPDATA", "/tmp/appdata" }, { "HOME", "/tmp/test_home" } }
    };
    auto const config = configHome(environment);
    REQUIRE(config.has_value());
    CHECK(*config == std::filesystem::path("/tmp/appdata"));
}

TEST_CASE("configHome.falls_back_to_home_dot_config", "[platform]")
{
    auto const environment = FakeEnvironment { { { "HOME", "/tmp/test_home" } } };
    auto const config = configHome(environment);
    REQUIRE(config.has_value());
    CHECK(*config == std::filesystem::path("/tmp/test_home/.config"));
}

TEST_CASE("configHome.nothing_to_derive_it_from", "[platform]")
{
    CHECK(!configHome(FakeEnvironment {}).has_value());
}

TEST_CASE("userName.prefers_USER", "[platform]")
{
    auto const environment =
        FakeEnvironment { { { "USER", "alice" }, { "LOGNAME", "bob" }, { "USERNAME", "carol" } } };
    CHECK(userName(environment) == "alice");
}

TEST_CASE("userName.falls_back_to_LOGNAME", "[platform]")
{
    auto const environment = FakeEnvironment { { { "LOGNAME", "bob" }, { "USERNAME", "carol" } } };
    CHECK(userName(environment) == "bob");
}

TEST_CASE("userName.falls_back_to_USERNAME", "[platform]")
{
    CHECK(userName(FakeEnvironment { { { "USERNAME", "carol" } } }) == "carol");
}

TEST_CASE("userName.skips_empty_values", "[platform]")
{
    CHECK(userName(FakeEnvironment { { { "USER", "" }, { "USERNAME", "carol" } } }) == "carol");
}

TEST_CASE("userName.none_set", "[platform]")
{
    CHECK(!userName(FakeEnvironment {}).has_value());
}

// Called without an environment, each is the same function over its default argument, the
// process environment. That the call exists is checked here without making it: every case
// above runs the one body, over a fake, and none depends on what the machine running it holds.
static_assert(requires { homeDirectory(); });
static_assert(requires { configHome(); });
static_assert(requires { userName(); });
