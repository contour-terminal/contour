// SPDX-License-Identifier: Apache-2.0
#include <crispy/environment.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("environment.get resolves names the way the host getenv() does", "[environment]")
{
    // The snapshot is keyed by a comparator that has to mirror the platform's own name resolution.
    // Windows preserves whatever casing the creating process used but matches case-insensitively, so
    // a byte-wise map answers nullopt for a `LOCALAPPDATA` lookup against a block spelling it
    // `LocalAppData` -- and callers that fall back on a missing variable (xdgStateHome() drops to the
    // temp directory) silently lose the user's state. POSIX resolves names byte-wise, and must keep
    // treating two spellings as two distinct variables.
    //
    // PATH is the one name that exists on every platform this builds for. The snapshot is immutable
    // and taken on first access, so the test reads only what the environment already held.
    auto const path = crispy::environment::get("PATH");
    REQUIRE(path.has_value());
    CHECK(!path->empty());

#ifdef _WIN32
    SECTION("Windows resolves names case-insensitively")
    {
        auto const lower = crispy::environment::get("path");
        auto const mixed = crispy::environment::get("Path");
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
        if (auto const lower = crispy::environment::get("path"); lower.has_value())
            CHECK(lower != path);
    }
#endif

    SECTION("an unset name yields nullopt")
    {
        CHECK(!crispy::environment::get("CONTOUR_DEFINITELY_UNSET_VARIABLE_NAME").has_value());
    }

    SECTION("getCString agrees with get")
    {
        auto const* const cstr = crispy::environment::getCString("PATH");
        REQUIRE(cstr != nullptr);
        CHECK(std::string { cstr } == std::string { *path });
        CHECK(crispy::environment::getCString("CONTOUR_DEFINITELY_UNSET_VARIABLE_NAME") == nullptr);
    }
}
