// SPDX-License-Identifier: Apache-2.0

#include <contour/config/Actions.hpp>
#include <contour/config/ChromeDensity.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace contour::window;
using contour::config::ChromeDensity;
using contour::config::PaneActionsReveal;

namespace
{
constexpr auto Base = PaneHeaderInputs {
    .visiblePanes = 2, .width = 400, .height = 300, .headerHeight = 22, .minimumContentHeight = 17
};

/// Every enumerator of @p E from zero up to and including @p last, so a loop over it visits them all.
template <typename E>
std::vector<E> enumerators(E last)
{
    auto result = std::vector<E> {};
    for (auto const i: std::views::iota(size_t { 0 }, static_cast<size_t>(last) + 1))
        result.push_back(static_cast<E>(i));
    return result;
}

} // namespace

TEST_CASE("paneHeader.singlePaneNeverHasAHeader", "[paneheader]")
{
    for (auto const density: enumerators(ChromeDensity::Minimal))
        for (auto const reveal: enumerators(HeaderReveal::HeldModifier))
            for (auto const pointer: enumerators(PointerPresence::Hovering))
                for (auto const focus: enumerators(PaneFocus::KeyboardFocused))
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
    constexpr auto Expected = std::array {
        PaneHeaderActionRow { PaneHeaderAction::SplitRight, "SplitVertical", "Split right" },
        PaneHeaderActionRow { PaneHeaderAction::SplitDown, "SplitHorizontal", "Split down" },
        PaneHeaderActionRow { PaneHeaderAction::Zoom, "TogglePaneZoom", "Zoom pane" },
        PaneHeaderActionRow { PaneHeaderAction::Close, "ClosePane", "Close pane" },
    };
    REQUIRE(PaneHeaderActions.size() == Expected.size());
    REQUIRE(enumerators(PaneHeaderAction::Close).size() == PaneHeaderActions.size());

    for (auto const i: std::views::iota(size_t { 0 }, PaneHeaderActions.size()))
    {
        auto const& row = PaneHeaderActions[i];
        INFO("row " << i);
        CHECK(row.action == Expected[i].action);
        CHECK(row.commandName == Expected[i].commandName);
        CHECK(row.accessibleName == Expected[i].accessibleName);
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
    auto const contents = enumerators(PaneHeaderContent::NameAndCwd);
    constexpr auto ContentNames = std::array<std::string_view, 3> { "hidden", "name", "nameAndCwd" };
    REQUIRE(contents.size() == ContentNames.size());
    for (auto const content: contents)
        CHECK(paneHeaderContentName(content) == ContentNames[static_cast<size_t>(content)]);

    auto const actions = enumerators(HeaderActions::Shown);
    constexpr auto ActionNames = std::array<std::string_view, 2> { "hidden", "shown" };
    REQUIRE(actions.size() == ActionNames.size());
    for (auto const actionsState: actions)
        CHECK(headerActionsName(actionsState) == ActionNames[static_cast<size_t>(actionsState)]);

    auto const placements = enumerators(HeaderPlacement::Overlay);
    constexpr auto PlacementNames = std::array<std::string_view, 2> { "inline", "overlay" };
    REQUIRE(placements.size() == PlacementNames.size());
    for (auto const placement: placements)
        CHECK(headerPlacementName(placement) == PlacementNames[static_cast<size_t>(placement)]);

    auto const emphases = enumerators(HeaderEmphasis::Primary);
    constexpr auto EmphasisNames = std::array<std::string_view, 2> { "secondary", "primary" };
    REQUIRE(emphases.size() == EmphasisNames.size());
    for (auto const emphasis: emphases)
        CHECK(headerEmphasisName(emphasis) == EmphasisNames[static_cast<size_t>(emphasis)]);

    // Densities are named by their table, so the name function and the table cannot disagree.
    auto const values = contour::config::configEnumValues<ChromeDensity>();
    REQUIRE(values.size() == contour::config::ChromeDensityCount);
    REQUIRE(enumerators(ChromeDensity::Minimal).size() == values.size());
    for (auto const i: std::views::iota(size_t { 0 }, values.size()))
    {
        CHECK(values[i].value == enumerators(ChromeDensity::Minimal)[i]);
        CHECK(contour::config::chromeDensityName(values[i].value) == values[i].token);
    }
    CHECK(contour::config::chromeDensityName(ChromeDensity::Minimal) == "minimal");
}

TEST_CASE("paneHeader.activePaneFocusByActivation", "[paneheader]")
{
    CHECK(activePaneFocus(PaneActivation::Pointer) == PaneFocus::Focused);
    CHECK(activePaneFocus(PaneActivation::Keyboard) == PaneFocus::KeyboardFocused);
    // A zero activation, as a tab that was never told one holds, is the pointer's.
    CHECK(activePaneFocus(PaneActivation {}) == PaneFocus::Focused);
    CHECK(ActivePaneFocus.size() == PaneActivationCount);
}
