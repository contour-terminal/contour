// SPDX-License-Identifier: Apache-2.0
#include <crispy/environment.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <cstdlib>
#endif

namespace
{

/// Sets or clears a variable in the process environment, so the live/snapshot distinction can be
/// observed. Writes the Win32 block on Windows, which is what live_environment reads there.
/// @param name Name of the variable to write.
/// @param value Its new value, or nullptr to unset it.
void writeEnvironmentVariable(char const* name, char const* value)
{
#ifdef _WIN32
    SetEnvironmentVariableA(name, value);
#else
    if (value != nullptr)
        ::setenv(name, value, 1);
    else
        ::unsetenv(name);
#endif
}

/// Test double standing in for a real environment, as production code injects one.
class fake_environment final: public crispy::environment
{
  public:
    explicit fake_environment(std::optional<std::string> answer): _answer { std::move(answer) } {}

    [[nodiscard]] std::optional<std::string> get(std::string_view /*name*/) const override { return _answer; }

  private:
    std::optional<std::string> _answer;
};

} // namespace

TEST_CASE("snapshot_environment resolves names the way the host getenv() does", "[environment]")
{
    // The snapshot is keyed by a comparator that has to mirror the platform's own name resolution.
    // Windows preserves whatever casing the creating process used but matches case-insensitively, so
    // a byte-wise map answers nullopt for a `LOCALAPPDATA` lookup against a block spelling it
    // `LocalAppData` -- and callers that fall back on a missing variable (xdgStateHome() drops to the
    // temp directory) silently lose the user's state. POSIX resolves names byte-wise, and must keep
    // treating two spellings as two distinct variables.
    //
    // PATH is the one name that exists on every platform this builds for.
    auto const environment = crispy::snapshot_environment {};

    auto const path = environment.get("PATH");
    REQUIRE(path.has_value());
    CHECK(!path->empty());

#ifdef _WIN32
    SECTION("Windows resolves names case-insensitively")
    {
        auto const lower = environment.get("path");
        auto const mixed = environment.get("Path");
        REQUIRE(lower.has_value());
        REQUIRE(mixed.has_value());
        CHECK(*lower == *path);
        CHECK(*mixed == *path);
    }
#else
    SECTION("POSIX resolves names byte-wise")
    {
        // Real environments spell it PATH; the lowercase name is a different variable and normally
        // absent. Asserting only that it is not silently aliased keeps this robust either way.
        if (auto const lower = environment.get("path"); lower.has_value())
            CHECK(lower != path);
    }
#endif

    SECTION("an unset name yields nullopt")
    {
        CHECK(!environment.get("CONTOUR_DEFINITELY_UNSET_VARIABLE_NAME").has_value());
    }
}

TEST_CASE("live_environment observes what a snapshot cannot", "[environment]")
{
    // The whole reason the live reader exists: config (re)loading expands `${VAR}` against the
    // environment as it stands at that moment, not as it stood when the process started.
    auto constexpr Name = "CONTOUR_ENVIRONMENT_TEST_VARIABLE";
    writeEnvironmentVariable(Name, nullptr);

    auto const snapshot = crispy::snapshot_environment {};
    auto const live = crispy::live_environment {};

    REQUIRE(!snapshot.get(Name).has_value());
    REQUIRE(!live.get(Name).has_value());

    writeEnvironmentVariable(Name, "observed");

    CHECK(!snapshot.get(Name).has_value());
    REQUIRE(live.get(Name).has_value());
    CHECK(live.get(Name) == "observed");

    SECTION("and observes an unset just as promptly")
    {
        writeEnvironmentVariable(Name, nullptr);
        CHECK(!live.get(Name).has_value());
    }

    writeEnvironmentVariable(Name, nullptr);
}

TEST_CASE("environment is reached through the interface", "[environment]")
{
    // What every injecting caller does: hold the abstraction, not an implementation.
    auto const substitute = fake_environment { "substituted" };
    auto const& injected = static_cast<crispy::environment const&>(substitute);

    CHECK(injected.get("ANY_NAME_AT_ALL") == "substituted");

    SECTION("and the process-wide defaults answer through it too")
    {
        CHECK(crispy::defaultEnvironment().get("PATH").has_value());
        CHECK(crispy::defaultLiveEnvironment().get("PATH").has_value());
    }
}
