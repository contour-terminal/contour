// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/platform/ProcessEnvironment.hpp>
#include <core/platform/UserPaths.hpp>
#include <core/platform/testing/TestProcessEnvironment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#ifdef _WIN32
    #include <core/platform/windows/WindowsProcessEnvironment.hpp>
#else
    #include <core/platform/posix/PosixProcessEnvironment.hpp>
#endif

using namespace core::platform;

// Spelled the way AGENT.md's namespace-equals-directory rule says a header in platform/testing/
// must be reachable, and the way its two neighbours there already are. A consumer writing the
// qualified name by analogy gets exactly this line.
using core::platform::testing::TestProcessEnvironment;

namespace
{
#ifdef _WIN32
using NativeProcessEnvironment = WindowsProcessEnvironment;
#else
using NativeProcessEnvironment = PosixProcessEnvironment;
#endif

/// Reads @p name through the read seam alone, as code that only reads is written against it.
[[nodiscard]] std::optional<std::string> readThroughSeam(core::Environment const& environment,
                                                         std::string_view name)
{
    return environment.get(name);
}

/// @return Why @p result failed, or std::nullopt where it did not -- so that a case asserting a
///         refusal fails on a success rather than reading an error that is not there.
[[nodiscard]] std::optional<PlatformError> refusalOf(std::expected<void, PlatformError> const& result)
{
    if (result.has_value())
        return std::nullopt;
    return result.error();
}
} // namespace

TEST_CASE("TestProcessEnvironment.set_and_get", "[platform]")
{
    TestProcessEnvironment env;
    REQUIRE(env.set("FOO", "bar").has_value());
    CHECK(env.get("FOO") == "bar");
}

TEST_CASE("TestProcessEnvironment.get_missing", "[platform]")
{
    TestProcessEnvironment const env;
    CHECK(!env.get("MISSING").has_value());
}

TEST_CASE("TestProcessEnvironment.unset", "[platform]")
{
    TestProcessEnvironment env;
    REQUIRE(env.set("KEY", "value").has_value());
    REQUIRE(env.unset("KEY").has_value());
    CHECK(!env.get("KEY").has_value());
}

TEST_CASE("TestProcessEnvironment.keys", "[platform]")
{
    TestProcessEnvironment env;
    REQUIRE(env.set("A", "1").has_value());
    REQUIRE(env.setAndExport("B", "2").has_value());
    auto const keys = env.keys();
    CHECK(keys.size() == 2);
}

TEST_CASE("TestProcessEnvironment is the one double for both seams", "[platform]")
{
    // A ProcessEnvironment IS a core::Environment: code that only reads takes the read seam, code
    // that writes takes this one, and a test that has both hands them the same object -- so there
    // is no second double to keep agreeing with the first (core-cpp#7).
    TestProcessEnvironment env { { { "HOME", "/home/alice" } } };
    REQUIRE(env.set("USER", "alice").has_value());
    CHECK(readThroughSeam(env, "USER") == "alice");
    CHECK(homeDirectory(env) == std::filesystem::path("/home/alice"));
    CHECK(userName(env) == "alice");
}

TEST_CASE("a ProcessEnvironment refuses a name no environment can hold, and says so", "[platform]")
{
    // set/unset returned void, so a name the process environment could not take was dropped
    // silently, and the variable a shell was told to export simply never appeared.
    auto const native = nativeProcessEnvironment();
    auto fake = TestProcessEnvironment {};
    for (auto* const env:
         { static_cast<ProcessEnvironment*>(native.get()), static_cast<ProcessEnvironment*>(&fake) })
    {
        CHECK(refusalOf(env->set("", "value")) == PlatformError::InvalidArgument);
        CHECK(refusalOf(env->set("A=B", "value")) == PlatformError::InvalidArgument);
        CHECK(refusalOf(env->set("CORE_CPP_NUL_VALUE", std::string_view { "a\0b", 3 }))
              == PlatformError::InvalidArgument);
        CHECK(refusalOf(env->unset("A=B")) == PlatformError::InvalidArgument);
        CHECK(refusalOf(env->setAndExport("", "value")) == PlatformError::InvalidArgument);
        CHECK(!env->get("A=B").has_value());
    }
}

TEST_CASE("nativeProcessEnvironment is this platform's own", "[platform]")
{
    // The implementations are private (posix/, windows/); a composition root reaches them only
    // through the factory.
    auto const environment = nativeProcessEnvironment();
    REQUIRE(environment != nullptr);
    CHECK(dynamic_cast<NativeProcessEnvironment const*>(environment.get()) != nullptr);
}

TEST_CASE("the native ProcessEnvironment exports what it was set to", "[platform]")
{
    // The one implementation that writes the process environment, the one a child process
    // inherits and LiveEnvironment reads. A variable set but not exported stays its own.
    constexpr auto Name = "CORE_CPP_NATIVE_PROVIDER_TEST_VARIABLE";
    auto const environment = nativeProcessEnvironment();
    REQUIRE(environment != nullptr);
    auto const live = core::LiveEnvironment {};

    REQUIRE(environment->set(Name, "local").has_value());
    CHECK(environment->get(Name) == "local");
    CHECK(!live.get(Name).has_value());

    REQUIRE(environment->exportVariable(Name).has_value());
    CHECK(live.get(Name) == "local");
    auto const keys = environment->keys();
    CHECK(std::ranges::find(keys, std::string { Name }) != keys.end());

    REQUIRE(environment->unset(Name).has_value());
    CHECK(!environment->get(Name).has_value());
    CHECK(!live.get(Name).has_value());
}

TEST_CASE("the native ProcessEnvironment reads what the process environment holds", "[platform]")
{
    // PATH exists on every platform this builds for, and the environment was never told it.
    auto const environment = nativeProcessEnvironment();
    REQUIRE(environment != nullptr);
    CHECK(environment->get("PATH") == core::LiveEnvironment {}.get("PATH"));
}

namespace
{
/// Removes a variable this file wrote, however the test case leaves.
///
/// The process environment is global to the binary, so a REQUIRE that throws past the cleanup
/// would leave the variable set for every test that runs afterwards.
struct WrittenVariable
{
    char const* name;
    ~WrittenVariable() { std::ignore = core::unsetProcessEnvironmentVariable(name); }
};
} // namespace

TEST_CASE("the native ProcessEnvironment reads an empty variable as set, not as absent", "[platform]")
{
    // An empty value is a variable that is set, which is what core::LiveEnvironment answers and
    // what Environment_test asserts of the writer. The Windows implementation hand-rolled its own
    // GetEnvironmentVariableA call instead of delegating, and a return of 0 there cannot tell an
    // empty value from a missing name -- so two readers of the same Win32 block disagreed.
    constexpr auto Name = "CORE_CPP_NATIVE_PROVIDER_EMPTY_TEST_VARIABLE";
    auto const cleanup = WrittenVariable { .name = Name };
    auto const environment = nativeProcessEnvironment();
    REQUIRE(environment != nullptr);

    REQUIRE(core::setProcessEnvironmentVariable(Name, "").has_value());
    REQUIRE(core::LiveEnvironment {}.get(Name) == "");
    CHECK(environment->get(Name) == "");

    REQUIRE(core::unsetProcessEnvironmentVariable(Name).has_value());
    CHECK(!environment->get(Name).has_value());
}

TEST_CASE("the native ProcessEnvironment reads a value longer than a page", "[platform]")
{
    // A reader that sizes a fixed buffer and ignores the "too small" return hands back a string
    // of NUL bytes instead of the value.
    constexpr auto Name = "CORE_CPP_NATIVE_PROVIDER_LONG_TEST_VARIABLE";
    auto const cleanup = WrittenVariable { .name = Name };
    auto const environment = nativeProcessEnvironment();
    REQUIRE(environment != nullptr);

    auto const value = std::string(8192, 'v');
    REQUIRE(core::setProcessEnvironmentVariable(Name, value).has_value());
    CHECK(environment->get(Name) == value);
}
