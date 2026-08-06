// SPDX-License-Identifier: Apache-2.0
#include <crispy/Environment.hpp>
#include <crispy/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
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
/// rejects them outright (concurrency-mt-unsafe). What LiveEnvironment consults on POSIX is the
/// environment block itself, so the test installs its own -- the original entries plus one -- and
/// restores the original pointer on the way out. Windows keeps a separate block that
/// SetEnvironmentVariableA writes and LiveEnvironment reads, so there it simply writes that.
class ScopedVariable
{
  public:
    /// @param name Name of the variable to make visible.
    /// @param value The value it should read as.
    ScopedVariable(std::string name, std::string const& value): _name { std::move(name) }
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

    ~ScopedVariable()
    {
#ifdef _WIN32
        SetEnvironmentVariableA(_name.c_str(), nullptr);
#else
        processEnviron() = _saved;
#endif
    }

    ScopedVariable(ScopedVariable const&) = delete;
    ScopedVariable& operator=(ScopedVariable const&) = delete;
    ScopedVariable(ScopedVariable&&) = delete;
    ScopedVariable& operator=(ScopedVariable&&) = delete;

  private:
    std::string _name;
#ifndef _WIN32
    std::string _entry;
    /// The block to put back, captured before the constructor body installs its own.
    char** _saved = processEnviron();
    std::vector<char*> _block;
#endif
};

} // namespace

TEST_CASE("LiveEnvironment resolves names the way the host getenv() does", "[environment]")
{
    // Name resolution is the platform's, not a comparator of ours: Windows preserves whatever casing
    // the creating process used but matches case-insensitively, so a `LOCALAPPDATA` lookup must find
    // a block that spells it `LocalAppData` -- get that wrong and every Windows user's state lands
    // in the fallback (a temp directory). POSIX resolves names byte-wise, and must keep treating two
    // spellings as two distinct variables.
    //
    // PATH is the one name that exists on every platform this builds for.
    auto const environment = crispy::LiveEnvironment {};

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

TEST_CASE("LiveEnvironment observes what a cache cannot", "[environment]")
{
    // The whole reason the reader is live: config (re)loading expands `${VAR}` against the
    // environment as it stands at that moment, not as it stood when the process started.
    auto constexpr Name = "CONTOUR_ENVIRONMENT_TEST_VARIABLE";

    auto const live = crispy::LiveEnvironment {};
    auto const cached = crispy::CachingEnvironment { live };

    // Read through the cache before the variable exists, so the miss is the answer it remembers.
    REQUIRE(!cached.get(Name).has_value());
    REQUIRE(!live.get(Name).has_value());

    {
        auto const variable = ScopedVariable { Name, "observed" };

        CHECK(!cached.get(Name).has_value());
        REQUIRE(live.get(Name).has_value());
        CHECK(live.get(Name) == "observed");
    }

    // ...and it observes the removal just as promptly.
    CHECK(!live.get(Name).has_value());
}

TEST_CASE("CachingEnvironment reads a name once", "[environment]")
{
    auto constexpr Name = "CONTOUR_ENVIRONMENT_TEST_VARIABLE";

    auto const live = crispy::LiveEnvironment {};
    auto const cached = crispy::CachingEnvironment { live };

    {
        auto const variable = ScopedVariable { Name, "first" };
        REQUIRE(cached.get(Name) == "first");
    }

    // The variable is gone from the block, yet the cache still answers -- which is exactly the
    // "frozen for the process's lifetime" property its users depend on.
    CHECK(cached.get(Name) == "first");
}

TEST_CASE("environment is reached through the interface", "[environment]")
{
    // What every injecting caller does: hold the abstraction, not an implementation.
    auto const substitute = crispy::testing::FakeEnvironment { { { "SOME_NAME", "substituted" } } };
    auto const& injected = static_cast<crispy::Environment const&>(substitute);

    CHECK(injected.get("SOME_NAME") == "substituted");
    CHECK(!injected.get("ANY_OTHER_NAME").has_value());

    SECTION("and the process-wide default answers through it too")
    {
        CHECK(crispy::defaultEnvironment().get("PATH").has_value());
    }
}
