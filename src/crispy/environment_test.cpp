// SPDX-License-Identifier: Apache-2.0
#include <crispy/environment.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <vector>

    #ifdef __APPLE__
        #include <crt_externs.h>
    #else
extern "C" char** environ;
    #endif
#endif

namespace
{

#ifndef _WIN32
/// @return The process's environment block, as the readers under test see it.
[[nodiscard]] char**& processEnviron() noexcept
{
    #ifdef __APPLE__
    return *_NSGetEnviron();
    #else
    return environ;
    #endif
}
#endif

/// Makes a variable visible to the readers under test for as long as it is alive, and puts the
/// environment back afterwards.
///
/// Not setenv()/unsetenv(): both are thread-unsafe, and this project's clang-tidy configuration
/// rejects them outright (concurrency-mt-unsafe). What live_environment consults on POSIX is the
/// environment block itself, so the test installs its own -- the original entries plus one -- and
/// restores the original pointer on the way out. Windows keeps a separate block that
/// SetEnvironmentVariableA writes and live_environment reads, so there it simply writes that.
class scoped_variable
{
  public:
    /// @param name Name of the variable to make visible.
    /// @param value The value it should read as.
    scoped_variable(std::string name, std::string const& value): _name { std::move(name) }
    {
#ifdef _WIN32
        SetEnvironmentVariableA(_name.c_str(), value.c_str());
#else
        // Appended to a copy of the existing block rather than replacing it, so everything else the
        // process (and the rest of this suite) reads from the environment survives.
        _entry = _name + "=" + value;
        for (char** e = _saved; e != nullptr && *e != nullptr; ++e)
            _block.push_back(*e);
        _block.push_back(_entry.data());
        _block.push_back(nullptr);
        processEnviron() = _block.data();
#endif
    }

    ~scoped_variable()
    {
#ifdef _WIN32
        SetEnvironmentVariableA(_name.c_str(), nullptr);
#else
        processEnviron() = _saved;
#endif
    }

    scoped_variable(scoped_variable const&) = delete;
    scoped_variable& operator=(scoped_variable const&) = delete;
    scoped_variable(scoped_variable&&) = delete;
    scoped_variable& operator=(scoped_variable&&) = delete;

  private:
    std::string _name;
#ifndef _WIN32
    std::string _entry;
    /// The block to put back, captured before the constructor body installs its own.
    char** _saved = processEnviron();
    std::vector<char*> _block;
#endif
};

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

    // Both taken before the variable exists, so the snapshot can never have seen it.
    auto const snapshot = crispy::snapshot_environment {};
    auto const live = crispy::live_environment {};

    REQUIRE(!snapshot.get(Name).has_value());
    REQUIRE(!live.get(Name).has_value());

    {
        auto const variable = scoped_variable { Name, "observed" };

        CHECK(!snapshot.get(Name).has_value());
        REQUIRE(live.get(Name).has_value());
        CHECK(live.get(Name) == "observed");
    }

    // ...and it observes the removal just as promptly.
    CHECK(!live.get(Name).has_value());
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
