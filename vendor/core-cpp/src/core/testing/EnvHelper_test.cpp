// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/testing/EnvHelper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace
{
constexpr auto Name = "CORE_CPP_ENV_HELPER_TEST_VARIABLE";
} // namespace

TEST_CASE("setTestEnv and unsetTestEnv change what the process environment says", "[testing][env]")
{
    // LiveEnvironment reads the environment the operating system holds for the process, the one a
    // child process inherits; setTestEnv must reach that one, not only a copy of it.
    auto const live = core::LiveEnvironment {};

    core::testing::setTestEnv(Name, "set");
    CHECK(live.get(Name) == "set");

    core::testing::unsetTestEnv(Name);
    CHECK(!live.get(Name).has_value());
}

TEST_CASE("ScopedEnv restores the value it replaced", "[testing][env]")
{
    auto const live = core::LiveEnvironment {};
    core::testing::setTestEnv(Name, "outer");
    {
        auto const scoped = core::testing::ScopedEnv { Name, "inner" };
        CHECK(live.get(Name) == "inner");
    }
    CHECK(live.get(Name) == "outer");
    core::testing::unsetTestEnv(Name);
}

TEST_CASE("ScopedEnv removes a variable that was not set before it", "[testing][env]")
{
    auto const live = core::LiveEnvironment {};
    REQUIRE(!live.get(Name).has_value());
    {
        auto const scoped = core::testing::ScopedEnv { Name, "temporary" };
        CHECK(live.get(Name) == "temporary");
    }
    CHECK(!live.get(Name).has_value());
}

TEST_CASE("setTestEnv sets an empty value rather than removing the variable", "[testing][env]")
{
    // An empty value is a variable that is set, which is what LiveEnvironment answers and what
    // Environment_test asserts of the writer. On Windows _putenv_s(name, "") removes the
    // variable instead, in the CRT's copy and in the Win32 block both.
    auto const live = core::LiveEnvironment {};

    core::testing::setTestEnv(Name, "");
    CHECK(live.get(Name) == "");

    core::testing::unsetTestEnv(Name);
    CHECK(!live.get(Name).has_value());
}

TEST_CASE("ScopedEnv restores a previous value that was empty", "[testing][env]")
{
    // The environment is process-global, so a destructor that removes a variable it was meant to
    // put back does not just fail this test: it changes what every test after it reads.
    auto const live = core::LiveEnvironment {};
    core::testing::setTestEnv(Name, "");
    REQUIRE(live.get(Name) == "");
    {
        auto const scoped = core::testing::ScopedEnv { Name, "inner" };
        CHECK(live.get(Name) == "inner");
    }
    CHECK(live.get(Name) == "");

    core::testing::unsetTestEnv(Name);
    CHECK(!live.get(Name).has_value());
}
