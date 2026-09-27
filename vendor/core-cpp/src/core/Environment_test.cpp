// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <algorithm>
    #include <cstddef>
    #include <string_view>
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
        auto* const* entry = _saved;
        while (entry != nullptr && *entry != nullptr)
        {
            _block.push_back(*entry);
            ++entry;
        }
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
    auto const environment = core::LiveEnvironment {};

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
        CHECK(!environment.get("CORE_CPP_DEFINITELY_UNSET_VARIABLE_NAME").has_value());
    }
}

TEST_CASE("LiveEnvironment observes what a cache cannot", "[environment]")
{
    // The whole reason the reader is live: config (re)loading expands `${VAR}` against the
    // environment as it stands at that moment, not as it stood when the process started.
    auto constexpr Name = "CORE_CPP_ENVIRONMENT_TEST_VARIABLE";

    auto const live = core::LiveEnvironment {};
    auto const cached = core::CachingEnvironment { live };

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
    auto constexpr Name = "CORE_CPP_ENVIRONMENT_TEST_VARIABLE";

    auto const live = core::LiveEnvironment {};
    auto const cached = core::CachingEnvironment { live };

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
    auto const substitute = core::testing::FakeEnvironment { { { "SOME_NAME", "substituted" } } };
    auto const& injected = static_cast<core::Environment const&>(substitute);

    CHECK(injected.get("SOME_NAME") == "substituted");
    CHECK(!injected.get("ANY_OTHER_NAME").has_value());

    SECTION("and the process-wide default answers through it too")
    {
        CHECK(core::defaultEnvironment().get("PATH").has_value());
    }
}

namespace
{
/// Removes a variable the process-environment writer set, however the test case ends.
class WrittenVariable
{
  public:
    /// @param name Name of the variable to remove when this goes out of scope.
    explicit WrittenVariable(std::string name): _name { std::move(name) } {}

    ~WrittenVariable() { std::ignore = core::unsetProcessEnvironmentVariable(_name); }

    WrittenVariable(WrittenVariable const&) = delete;
    WrittenVariable& operator=(WrittenVariable const&) = delete;
    WrittenVariable(WrittenVariable&&) = delete;
    WrittenVariable& operator=(WrittenVariable&&) = delete;

  private:
    std::string _name;
};

#ifndef _WIN32
/// @return A copy of every entry of @p block, an environment block as `environ` points at one.
[[nodiscard]] std::vector<std::string> entriesOf(char* const* block)
{
    auto entries = std::vector<std::string> {};
    while (block != nullptr && *block != nullptr)
    {
        entries.emplace_back(*block);
        ++block;
    }
    return entries;
}

/// Installs a copy of the environment block with entries appended for as long as it is alive, and
/// puts the original block back afterwards, whatever the writer published in between. The way to
/// hand the writer a block it could otherwise only have inherited, such as one naming a variable
/// twice.
class InstalledBlock
{
  public:
    /// @param extra "name=value" entries to append, in order.
    explicit InstalledBlock(std::vector<std::string> extra): _extra { std::move(extra) }
    {
        auto* const* entry = _saved;
        while (entry != nullptr && *entry != nullptr)
        {
            _block.push_back(*entry);
            ++entry;
        }
        for (auto& line: _extra)
            _block.push_back(line.data());
        _block.push_back(nullptr);
        processEnviron() = _block.data();
    }

    ~InstalledBlock() { processEnviron() = _saved; }

    InstalledBlock(InstalledBlock const&) = delete;
    InstalledBlock& operator=(InstalledBlock const&) = delete;
    InstalledBlock(InstalledBlock&&) = delete;
    InstalledBlock& operator=(InstalledBlock&&) = delete;

  private:
    /// The block to put back, captured before the constructor body installs its own.
    char** _saved = processEnviron();
    std::vector<std::string> _extra;
    std::vector<char*> _block;
};

/// @return How many entries of the current environment block are for @p name.
[[nodiscard]] std::ptrdiff_t entriesNaming(std::string_view name)
{
    auto const prefix = std::string { name } + "=";
    return std::ranges::count_if(entriesOf(processEnviron()),
                                 [&](std::string const& line) { return line.starts_with(prefix); });
}
#endif
} // namespace

TEST_CASE("the process-environment writer is what LiveEnvironment then reads", "[environment]")
{
    auto constexpr Name = "CORE_CPP_ENVIRONMENT_WRITER_TEST_VARIABLE";
    auto const cleanup = WrittenVariable { Name };
    auto const live = core::LiveEnvironment {};

    REQUIRE(core::setProcessEnvironmentVariable(Name, "one").has_value());
    CHECK(live.get(Name) == "one");

    // A second write replaces the value rather than adding a second entry that shadows it.
    REQUIRE(core::setProcessEnvironmentVariable(Name, "two").has_value());
    CHECK(live.get(Name) == "two");

    // An empty value is a variable that is set, not one that is removed.
    REQUIRE(core::setProcessEnvironmentVariable(Name, "").has_value());
    CHECK(live.get(Name) == "");

    REQUIRE(core::unsetProcessEnvironmentVariable(Name).has_value());
    CHECK(!live.get(Name).has_value());

    // Removing what is not there is not an error: the variable is unset either way.
    CHECK(core::unsetProcessEnvironmentVariable(Name).has_value());
}

TEST_CASE("the process-environment writer refuses a name no environment can hold", "[environment]")
{
    auto const invalid = std::make_error_code(std::errc::invalid_argument);

    CHECK(core::setProcessEnvironmentVariable("", "value").error() == invalid);
    CHECK(core::setProcessEnvironmentVariable("A=B", "value").error() == invalid);
    CHECK(core::unsetProcessEnvironmentVariable("").error() == invalid);
    CHECK(core::unsetProcessEnvironmentVariable("A=B").error() == invalid);
}

#ifndef _WIN32
TEST_CASE("the process-environment writer leaves a block a reader holds intact", "[environment]")
{
    // getenv() in another library, or execvp(), walks `environ` without taking any lock of ours. So
    // the writer must never edit the block such a reader may hold, nor free it: it publishes a new
    // one. The block captured here has to read exactly as it did before the writes.
    auto constexpr Name = "CORE_CPP_ENVIRONMENT_WRITER_TEST_VARIABLE";
    auto const cleanup = WrittenVariable { Name };

    REQUIRE(core::setProcessEnvironmentVariable(Name, "before").has_value());
    auto* const* const held = processEnviron();
    auto const snapshot = entriesOf(held);

    REQUIRE(core::setProcessEnvironmentVariable(Name, "after").has_value());
    REQUIRE(core::unsetProcessEnvironmentVariable(Name).has_value());

    CHECK(processEnviron() != held);
    CHECK(entriesOf(held) == snapshot);
}
#endif

#ifndef _WIN32
TEST_CASE("the process-environment writer publishes nothing for a write that changes nothing",
          "[environment]")
{
    // Each published block is kept for the rest of the process, so a repeated identical export --
    // a program re-applying its configuration, say -- must not cost one every time.
    auto constexpr Name = "CORE_CPP_ENVIRONMENT_WRITER_TEST_VARIABLE";
    auto const cleanup = WrittenVariable { Name };

    REQUIRE(core::setProcessEnvironmentVariable(Name, "same").has_value());
    auto* const* const published = processEnviron();

    REQUIRE(core::setProcessEnvironmentVariable(Name, "same").has_value());
    CHECK(processEnviron() == published);

    // A different value, though, is a new block.
    REQUIRE(core::setProcessEnvironmentVariable(Name, "different").has_value());
    CHECK(processEnviron() != published);
    CHECK(core::LiveEnvironment {}.get(Name) == "different");
}

TEST_CASE("the process-environment writer publishes for a variable named twice", "[environment]")
{
    // A block inherited through execve() can name a variable twice. Readers -- LiveEnvironment and
    // getenv() alike -- take the first entry, so only a single entry that already reads the value
    // makes a write one that changes nothing. With a second entry the writer publishes, and the
    // block it publishes names the variable once.
    auto constexpr Name = "CORE_CPP_ENVIRONMENT_WRITER_TEST_VARIABLE";
    auto const live = core::LiveEnvironment {};
    auto const entry = [&](std::string_view value) {
        return std::string { Name } + "=" + std::string { value };
    };

    SECTION("the value only the second entry holds")
    {
        auto const installed = InstalledBlock { { entry("old"), entry("same") } };
        REQUIRE(live.get(Name) == "old");

        REQUIRE(core::setProcessEnvironmentVariable(Name, "same").has_value());
        CHECK(live.get(Name) == "same");
        CHECK(entriesNaming(Name) == 1);
    }

    SECTION("the value the first entry already holds")
    {
        auto const installed = InstalledBlock { { entry("same"), entry("old") } };
        auto* const* const before = processEnviron();

        REQUIRE(core::setProcessEnvironmentVariable(Name, "same").has_value());
        CHECK(processEnviron() != before);
        CHECK(live.get(Name) == "same");
        CHECK(entriesNaming(Name) == 1);
    }
}
#endif
