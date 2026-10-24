// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vthost/AccessFile.hpp>

using vtcontrol::ControlGrants;
using vtcontrol::Permission;

TEST_CASE("an access file yields one accepted token per line", "[vthost][grants]")
{
    auto const entries = vthost::parseAccessFile("# scripts\nread,screen s3cret one\n\nall admin\r\n");
    REQUIRE(entries);
    REQUIRE(entries->size() == 2);
    CHECK((*entries)[0].token == "s3cret one");
    CHECK((*entries)[0].grants == ControlGrants::of({ Permission::Read, Permission::Screen }));
    CHECK((*entries)[1].token == "admin");
    CHECK((*entries)[1].grants == ControlGrants::all());

    SECTION("an unknown grant names the line")
    {
        auto const unknown = vthost::parseAccessFile("fly t");
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error().contains("line 1"));
        CHECK(unknown.error().contains("'fly'"));
    }
    SECTION("an empty token is refused")
    {
        auto const empty = vthost::parseAccessFile("# none\nread ");
        REQUIRE_FALSE(empty);
        CHECK(empty.error().contains("line 2"));
        CHECK(empty.error().contains("token is empty"));
    }
    SECTION("a repeated token is refused")
    {
        auto const repeated = vthost::parseAccessFile("all one\nread two\nread,screen one\n");
        REQUIRE_FALSE(repeated);
        CHECK(repeated.error().contains("line 3"));
        CHECK(repeated.error().contains("line 1"));
    }
    SECTION("a line repeating --token is refused")
    {
        auto const repeated = vthost::parseAccessFile("read,screen ci\nread admin\n", "admin");
        REQUIRE_FALSE(repeated);
        CHECK(repeated.error().contains("line 2"));
        CHECK(repeated.error().contains("repeats --token"));
        CHECK(vthost::parseAccessFile("read,screen ci\n", "admin").has_value());
    }
    SECTION("indentation is not part of a line")
    {
        auto const indented = vthost::parseAccessFile("  # a note\n\tread,screen ci\n");
        REQUIRE(indented);
        REQUIRE(indented->size() == 1);
        CHECK(indented->front().token == "ci");
        CHECK(indented->front().grants == ControlGrants::of({ Permission::Read, Permission::Screen }));
    }
    SECTION("an empty file accepts no token")
    {
        auto const none = vthost::parseAccessFile("\n# nothing yet\n");
        REQUIRE(none);
        CHECK(none->empty());
    }
}

TEST_CASE("an access file is read through the file system", "[vthost][grants]")
{
    auto const files = core::platform::testing::InMemoryFileSystem { { .path = "/etc/contour/access",
                                                                       .content = "input typist\n" } };
    auto const entries = vthost::readAccessFile(files, "/etc/contour/access");
    REQUIRE(entries);
    REQUIRE(entries->size() == 1);
    CHECK(entries->front().grants == ControlGrants::of({ Permission::Input }));

    auto const missing = vthost::readAccessFile(files, "/etc/contour/nope");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().contains("/etc/contour/nope"));

    auto const bad = core::platform::testing::InMemoryFileSystem { { .path = "/a", .content = "fly t\n" } };
    auto const invalid = vthost::readAccessFile(bad, "/a");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().starts_with("/a: access file line 1"));

    auto const beside = vthost::readAccessFile(files, "/etc/contour/access", "typist");
    REQUIRE_FALSE(beside);
    CHECK(beside.error().contains("repeats --token"));
}
