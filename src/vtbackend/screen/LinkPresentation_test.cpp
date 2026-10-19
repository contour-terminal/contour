// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/LinkPresentation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

using namespace vtbackend;

TEST_CASE("LinkPresentation.tableNamesEveryCaseOnce", "[links]")
{
    for (auto const mode: { LinkDecorationMode::Always,
                            LinkDecorationMode::Hover,
                            LinkDecorationMode::Modifier,
                            LinkDecorationMode::Never })
        for (auto const pointer: { LinkPointer::Elsewhere, LinkPointer::Over })
            for (auto const modifiers: { FollowModifiers::Released, FollowModifiers::Held })
                CHECK(std::ranges::count_if(LinkPresentationTable,
                                            [&](LinkPresentationRow const& row) {
                                                return row.mode == mode && row.pointer == pointer
                                                       && row.modifiers == modifiers;
                                            })
                      == 1);
}

TEST_CASE("LinkPresentation.specTableRemainingRows", "[links]")
{
    using enum LinkDecorationRole;
    // always: the modifier changes nothing, and the hover is the same held or not.
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Always, LinkPointer::Elsewhere, FollowModifiers::Held)
                   == LinkPresentation { Normal, LinkPointerShape::Default });
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Always, LinkPointer::Over, FollowModifiers::Held)
                   == LinkPresentation { Hover, LinkPointerShape::Hand });
    // hover: nothing at rest; the one under the pointer is hovered, with or without the modifier.
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Hover, LinkPointer::Elsewhere, FollowModifiers::Released)
                   == LinkPresentation { None, LinkPointerShape::Default });
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Hover, LinkPointer::Over, FollowModifiers::Released)
                   == LinkPresentation { Hover, LinkPointerShape::Hand });
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Hover, LinkPointer::Over, FollowModifiers::Held)
                   == LinkPresentation { Hover, LinkPointerShape::Hand });
    // modifier: nothing without the modifier, wherever the pointer is.
    STATIC_REQUIRE(
        presentLink(LinkDecorationMode::Modifier, LinkPointer::Elsewhere, FollowModifiers::Released)
        == LinkPresentation { None, LinkPointerShape::Default });
    // never: no decoration and no hand for a link the pointer is not over.
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Never, LinkPointer::Elsewhere, FollowModifiers::Released)
                   == LinkPresentation { None, LinkPointerShape::Default });
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Never, LinkPointer::Elsewhere, FollowModifiers::Held)
                   == LinkPresentation { None, LinkPointerShape::Default });
}

TEST_CASE("LinkPresentation.specTable", "[links]")
{
    using enum LinkDecorationRole;
    // always: decorated at rest, hovered under the pointer.
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Always, LinkPointer::Elsewhere, FollowModifiers::Released)
                   == LinkPresentation { Normal, LinkPointerShape::Default });
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Always, LinkPointer::Over, FollowModifiers::Released)
                   == LinkPresentation { Hover, LinkPointerShape::Hand });
    // hover: only the one under the pointer.
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Hover, LinkPointer::Elsewhere, FollowModifiers::Held)
                   == LinkPresentation { None, LinkPointerShape::Default });
    // modifier: every link while the modifier is held, none without it.
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Modifier, LinkPointer::Over, FollowModifiers::Released)
                   == LinkPresentation { None, LinkPointerShape::Default });
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Modifier, LinkPointer::Elsewhere, FollowModifiers::Held)
                   == LinkPresentation { Normal, LinkPointerShape::Default });
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Modifier, LinkPointer::Over, FollowModifiers::Held)
                   == LinkPresentation { Hover, LinkPointerShape::Hand });
    // never: no decoration; the hand still says the click opens it.
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Never, LinkPointer::Over, FollowModifiers::Held)
                   == LinkPresentation { None, LinkPointerShape::Hand });
    STATIC_REQUIRE(presentLink(LinkDecorationMode::Never, LinkPointer::Over, FollowModifiers::Released)
                   == LinkPresentation { None, LinkPointerShape::Default });
}
