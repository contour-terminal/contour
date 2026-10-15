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

// A core::Environment answers UTF-8, on Windows by converting the wide environment block. Each
// variable below names a directory under C:\Users\Jürgen there, which the narrow constructor read
// back in the ANSI code page, so every one of them named a directory that does not exist.
TEST_CASE("homeDirectory.reads_the_variable_as_UTF-8", "[platform]")
{
    CHECK(homeDirectory(FakeEnvironment { { { "HOME", "/home/j\xC3\xBCrgen" } } })
          == std::filesystem::path { u8"/home/jürgen" });
    CHECK(homeDirectory(FakeEnvironment { { { "USERPROFILE", "C:\\Users\\J\xC3\xBCrgen" } } })
          == std::filesystem::path { u8"C:\\Users\\Jürgen" });
}

TEST_CASE("configHome.reads_the_variable_as_UTF-8", "[platform]")
{
    CHECK(configHome(FakeEnvironment { { { "XDG_CONFIG_HOME", "/home/j\xC3\xBCrgen/.config" } } })
          == std::filesystem::path { u8"/home/jürgen/.config" });
    CHECK(configHome(FakeEnvironment { { { "APPDATA", "C:\\Users\\J\xC3\xBCrgen\\AppData\\Roaming" } } })
          == std::filesystem::path { u8"C:\\Users\\Jürgen\\AppData\\Roaming" });
    CHECK(configHome(FakeEnvironment { { { "HOME", "/home/j\xC3\xBCrgen" } } })
          == std::filesystem::path { u8"/home/jürgen/.config" });
}

// A variable is text from outside the program, so one that is not UTF-8 -- "jürgen" in CP-1252
// here, where 0xFC starts no UTF-8 sequence -- names no directory on Windows and is passed over,
// as an unset one is. It threw there before. A POSIX path is bytes, so they are kept as they are.
TEST_CASE("homeDirectory.passes_over_a_variable_that_is_not_UTF-8", "[platform]")
{
    auto const environment =
        FakeEnvironment { { { "HOME", "/home/j\xFCrgen" }, { "USERPROFILE", "C:\\Users\\J\xC3\xBCrgen" } } };
#ifdef _WIN32
    CHECK(homeDirectory(environment) == std::filesystem::path { u8"C:\\Users\\Jürgen" });
    CHECK_FALSE(homeDirectory(FakeEnvironment { { { "HOME", "/home/j\xFCrgen" } } }).has_value());
#else
    CHECK(homeDirectory(environment).value().native() == "/home/j\xFCrgen");
#endif
}

TEST_CASE("configHome.passes_over_a_variable_that_is_not_UTF-8", "[platform]")
{
    auto const environment = FakeEnvironment { {
        { "XDG_CONFIG_HOME", "/home/j\xFCrgen/.config" },
        { "APPDATA", "C:\\Users\\J\xC3\xBCrgen\\AppData\\Roaming" },
    } };
#ifdef _WIN32
    CHECK(configHome(environment) == std::filesystem::path { u8"C:\\Users\\Jürgen\\AppData\\Roaming" });
    CHECK(configHome(FakeEnvironment {
              { { "APPDATA", "C:\\Users\\J\xFCrgen" }, { "HOME", "/home/j\xC3\xBCrgen" } } })
          == std::filesystem::path { u8"/home/jürgen/.config" });
#else
    CHECK(configHome(environment).value().native() == "/home/j\xFCrgen/.config");
#endif
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
