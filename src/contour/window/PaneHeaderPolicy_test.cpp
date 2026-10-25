// SPDX-License-Identifier: Apache-2.0

#include <contour/config/Actions.hpp>
#include <contour/config/ChromeDensity.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>

using namespace contour::window;
using contour::config::ChromeDensity;
using contour::config::PaneActionsReveal;

namespace
{
constexpr auto Base = PaneHeaderInputs {
    .visiblePanes = 2, .width = 400, .height = 300, .headerHeight = 22, .minimumContentHeight = 17
};

constexpr auto AllDensities = std::array { ChromeDensity::Comfortable, ChromeDensity::Minimal };
constexpr auto AllReveals = std::array { HeaderReveal::None, HeaderReveal::HeldModifier };
constexpr auto AllPointers = std::array { PointerPresence::Away, PointerPresence::Hovering };
constexpr auto AllFocus = std::array { PaneFocus::Unfocused, PaneFocus::Focused, PaneFocus::KeyboardFocused };
} // namespace

TEST_CASE("paneHeader.singlePaneNeverHasAHeader", "[paneheader]")
{
    for (auto const density: AllDensities)
        for (auto const reveal: AllReveals)
            for (auto const pointer: AllPointers)
                for (auto const focus: AllFocus)
                {
                    auto inputs = Base;
                    inputs.visiblePanes = 1;
                    inputs.density = density;
                    inputs.reveal = reveal;
                    inputs.pointer = pointer;
                    inputs.focus = focus;
                    CHECK(resolvePaneHeader(inputs) == PaneHeader {});
                }
}

TEST_CASE("paneHeader.comfortableSplit", "[paneheader]")
{
    CHECK(resolvePaneHeader(Base)
          == PaneHeader { PaneHeaderContent::NameAndCwd,
                          HeaderActions::Hidden,
                          HeaderPlacement::Inline,
                          HeaderEmphasis::Secondary });

    auto focused = Base;
    focused.focus = PaneFocus::Focused;
    CHECK(resolvePaneHeader(focused).emphasis == HeaderEmphasis::Primary);

    auto narrow = Base;
    narrow.width = 359.5;
    CHECK(resolvePaneHeader(narrow).content == PaneHeaderContent::Name);
    narrow.width = 360;
    CHECK(resolvePaneHeader(narrow).content == PaneHeaderContent::NameAndCwd);
}

TEST_CASE("paneHeader.actionsOnHover", "[paneheader]")
{
    auto inputs = Base;
    inputs.pointer = PointerPresence::Hovering;
    CHECK(resolvePaneHeader(inputs).actions == HeaderActions::Shown);

    inputs = Base;
    inputs.focus = PaneFocus::KeyboardFocused;
    CHECK(resolvePaneHeader(inputs).actions == HeaderActions::Shown);

    inputs = Base;
    inputs.focus = PaneFocus::Focused;
    CHECK(resolvePaneHeader(inputs).actions == HeaderActions::Hidden);

    inputs.actionsReveal = PaneActionsReveal::OnHoverOrFocus;
    CHECK(resolvePaneHeader(inputs).actions == HeaderActions::Shown);
}

TEST_CASE("paneHeader.minimalNeedsTheHeldModifier", "[paneheader]")
{
    auto inputs = Base;
    inputs.density = ChromeDensity::Minimal;
    CHECK(resolvePaneHeader(inputs) == PaneHeader {});

    inputs.reveal = HeaderReveal::HeldModifier;
    auto const header = resolvePaneHeader(inputs);
    CHECK(header.placement == HeaderPlacement::Overlay);
    CHECK(header.content == PaneHeaderContent::NameAndCwd);
}

TEST_CASE("paneHeader.tooShortHasNoHeader", "[paneheader]")
{
    auto inputs = Base;
    inputs.height = 38.9;
    CHECK(resolvePaneHeader(inputs) == PaneHeader {});
    inputs.height = 39;
    CHECK(resolvePaneHeader(inputs).content != PaneHeaderContent::Hidden);

    // An overlay takes no room, so a short pane still shows it.
    inputs.density = ChromeDensity::Minimal;
    inputs.reveal = HeaderReveal::HeldModifier;
    inputs.height = 10;
    CHECK(resolvePaneHeader(inputs).content != PaneHeaderContent::Hidden);
}

TEST_CASE("paneHeader.rulesCoverEveryDensity", "[paneheader]")
{
    for (auto const i: std::views::iota(size_t { 0 }, contour::config::ChromeDensityCount))
        CHECK(DensityHeaderRules[i].density == static_cast<ChromeDensity>(i));
}

TEST_CASE("paneHeader.actionsResolve", "[paneheader]")
{
    for (auto const i: std::views::iota(size_t { 0 }, PaneHeaderActions.size()))
    {
        auto const& row = PaneHeaderActions[i];
        CHECK(row.action == static_cast<PaneHeaderAction>(i));
        auto const action = contour::actions::fromString(std::string(row.commandName));
        REQUIRE(action.has_value());
        CHECK(contour::actions::name(*action) == row.commandName);
    }
}

TEST_CASE("paneHeader.name", "[paneheader]")
{
    CHECK(paneHeaderName("vim", "zsh") == "vim");
    CHECK(paneHeaderName("", "zsh") == "zsh");
}

TEST_CASE("paneHeader.names", "[paneheader]")
{
    CHECK(paneHeaderContentName(PaneHeaderContent::Hidden) == "hidden");
    CHECK(paneHeaderContentName(PaneHeaderContent::Name) == "name");
    CHECK(paneHeaderContentName(PaneHeaderContent::NameAndCwd) == "nameAndCwd");
    CHECK(headerActionsName(HeaderActions::Hidden) == "hidden");
    CHECK(headerActionsName(HeaderActions::Shown) == "shown");
    CHECK(headerPlacementName(HeaderPlacement::Inline) == "inline");
    CHECK(headerPlacementName(HeaderPlacement::Overlay) == "overlay");
    CHECK(headerEmphasisName(HeaderEmphasis::Secondary) == "secondary");
    CHECK(headerEmphasisName(HeaderEmphasis::Primary) == "primary");

    CHECK(contour::config::chromeDensityName(ChromeDensity::Comfortable) == "comfortable");
    CHECK(contour::config::chromeDensityName(ChromeDensity::Minimal) == "minimal");
    auto const values = contour::config::configEnumValues<ChromeDensity>();
    REQUIRE(values.size() == contour::config::ChromeDensityCount);
    CHECK(values[0].value == ChromeDensity::Comfortable);
    CHECK(values[1].value == ChromeDensity::Minimal);
}
