// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/testing/EnvHelper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <cstddef>
    #include <cstdlib> // _wgetenv_s
    #include <string_view>
#endif

namespace
{
constexpr auto Name = "CORE_CPP_ENV_HELPER_TEST_VARIABLE";

/// A name and a value beyond ASCII, in UTF-8: characters of two, three and four bytes, most of them
/// in no ANSI code page.
constexpr auto NameBeyondAscii = "CORE_CPP_ENV_HELPER_TEST_\xC3\x9C\xCE\xA9";
constexpr auto ValueBeyondAscii = "J\xC3\xBCrgen-\xE6\x97\xA5\xE6\x9C\xAC-\xF0\x9F\x98\x80";

#ifdef _WIN32
/// What the CRT's own copy of the environment holds -- not the Win32 block LiveEnvironment reads --
/// asked through the wide CRT, which spells it as it was written.
/// @param name The variable, in UTF-8.
/// @return Its value in UTF-8, or std::nullopt where the CRT's copy does not have it.
[[nodiscard]] std::optional<std::string> crtValue(std::string_view name)
{
    auto const wideName = core::toUtf16(name).value();
    auto required = std::size_t {};
    if (_wgetenv_s(&required, nullptr, 0, wideName.c_str()) != 0 || required == 0)
        return std::nullopt;
    auto value = std::wstring(required, L'\0');
    if (_wgetenv_s(&required, value.data(), value.size(), wideName.c_str()) != 0 || required == 0)
        return std::nullopt;
    value.resize(required - 1); // `required` counts the terminating NUL
    return core::toUtf8(value);
}
#endif
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

TEST_CASE("ScopedEnv sets a name and a value beyond ASCII as written", "[testing][env]")
{
    // Both are UTF-8, as every core-cpp string is. On Windows _putenv_s() read them in the ANSI code
    // page, so the value reached neither the Win32 block -- which LiveEnvironment reads and a child
    // process inherits -- nor the CRT's own copy as it was written.
    auto const live = core::LiveEnvironment {};
    REQUIRE(!live.get(NameBeyondAscii).has_value());
    {
        auto const scoped = core::testing::ScopedEnv { NameBeyondAscii, ValueBeyondAscii };
        CHECK(live.get(NameBeyondAscii) == ValueBeyondAscii);
#ifdef _WIN32
        CHECK(crtValue(NameBeyondAscii) == ValueBeyondAscii);
#endif
    }
    CHECK(!live.get(NameBeyondAscii).has_value());
#ifdef _WIN32
    CHECK(!crtValue(NameBeyondAscii).has_value());
#endif
}

TEST_CASE("ScopedEnv restores a previous value beyond ASCII as written", "[testing][env]")
{
    // The value it puts back is the one LiveEnvironment read, in UTF-8, and it goes back the way the
    // test's own did.
    auto const live = core::LiveEnvironment {};
    core::testing::setTestEnv(Name, ValueBeyondAscii);
    REQUIRE(live.get(Name) == ValueBeyondAscii);
    {
        auto const scoped = core::testing::ScopedEnv { Name, "inner" };
        CHECK(live.get(Name) == "inner");
    }
    CHECK(live.get(Name) == ValueBeyondAscii);
#ifdef _WIN32
    CHECK(crtValue(Name) == ValueBeyondAscii);
#endif

    core::testing::unsetTestEnv(Name);
    CHECK(!live.get(Name).has_value());
#ifdef _WIN32
    CHECK(!crtValue(Name).has_value());
#endif
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
