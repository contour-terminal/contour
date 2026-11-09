// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <vthost/SwitchClient.hpp>

namespace
{
using vthost::ConnectionId;
using vthost::SwitchClientError;

ConnectionId client(std::uint64_t index)
{
    return ConnectionId { .endpoint = "native", .index = index };
}
} // namespace

TEST_CASE("chooseSwitchClient picks the last typist, else the only client", "[vthost][switch]")
{
    auto const a = client(1);
    auto const b = client(2);
    auto const gone = client(3);

    SECTION("the last typist among several")
    {
        auto const showing = std::vector { a, b };
        auto const choice = vthost::chooseSwitchClient(showing, a);
        REQUIRE(choice.chosen.has_value());
        CHECK(*choice.chosen == a);
        CHECK(choice.candidates == showing);
        CHECK(*vthost::chooseSwitchClient(showing, b).chosen == b);
    }
    SECTION("a typist that shows nothing of it leaves several ambiguous")
    {
        auto const showing = std::vector { a, b };
        auto const choice = vthost::chooseSwitchClient(showing, gone);
        REQUIRE(!choice.chosen.has_value());
        CHECK(choice.chosen.error() == SwitchClientError::Ambiguous);
        CHECK(choice.candidates == showing);
    }
    SECTION("no typist known: several are ambiguous")
    {
        auto const showing = std::vector { a, b };
        auto const choice = vthost::chooseSwitchClient(showing, std::nullopt);
        REQUIRE(!choice.chosen.has_value());
        CHECK(choice.chosen.error() == SwitchClientError::Ambiguous);
    }
    SECTION("the only client, typist or not")
    {
        auto const showing = std::vector { a };
        CHECK(*vthost::chooseSwitchClient(showing, std::nullopt).chosen == a);
        CHECK(*vthost::chooseSwitchClient(showing, gone).chosen == a);
    }
    SECTION("none shows it")
    {
        auto const choice = vthost::chooseSwitchClient({}, a);
        REQUIRE(!choice.chosen.has_value());
        CHECK(choice.chosen.error() == SwitchClientError::NoClient);
        CHECK(choice.candidates.empty());
    }
}
