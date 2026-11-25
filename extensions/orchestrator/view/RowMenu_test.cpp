// SPDX-License-Identifier: Apache-2.0
#include <core/tui/KeyCode.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/NavKeys.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/NavInput.hpp>
#include <orchestrator/view/RowMenu.hpp>

using namespace orchestrator;
using namespace orchestrator::view;

namespace
{

/// A merge request's row, linked to PRJ-1234.
[[nodiscard]] WorkItem mrItem()
{
    return WorkItem { .group = WorkGroup::NeedsYou,
                      .repo = "contour",
                      .changeRequest = ChangeRequest { .number = 2375,
                                                       .url = "https://github.com/example/contour/pull/2375",
                                                       .head = "fix/flaky-test" },
                      .ticket = Issue { .item = JiraRef { .key = "PRJ-1234" },
                                        .url = "https://example.atlassian.net/browse/PRJ-1234" },
                      .ticketKey = "PRJ-1234",
                      .item = ChangeRequestRef { 2375 } };
}

/// A ticket's row with no merge request.
[[nodiscard]] WorkItem ticketItem()
{
    return WorkItem { .group = WorkGroup::NoMrYet,
                      .repo = "contour",
                      .ticket = Issue { .item = JiraRef { .key = "PRJ-1302" },
                                        .url = "https://example.atlassian.net/browse/PRJ-1302" },
                      .ticketKey = "PRJ-1302",
                      .item = JiraRef { .key = "PRJ-1302" } };
}

[[nodiscard]] core::tui::KeyEvent key(core::tui::KeyCode code)
{
    return core::tui::KeyEvent { .key = code };
}

/// Presses @p key on the menu as the dashboard does: resolved in Navigating by the default keys, a
/// sequence's first key held in @p pending until the next.
/// @return The command chosen, if any.
std::optional<RowMenuCommand> pressOn(core::tui::KeyEvent const& key,
                                      std::optional<RowMenuState>& menu,
                                      WorkItem const* item,
                                      std::string& pending)
{
    static auto const keys = NavKeys::defaults(VimKeys::On);
    auto const nav = navActionOf(key, NavContext::Navigating, keys, pending);
    if (!nav && nav.error() == NavMiss::Pending)
        return std::nullopt;
    return applyRowMenuKey(key, nav ? std::optional { *nav } : std::nullopt, menu, item);
}

/// Presses @p key on the menu, with no key pending before it.
std::optional<RowMenuCommand> pressOn(core::tui::KeyEvent const& key,
                                      std::optional<RowMenuState>& menu,
                                      WorkItem const* item)
{
    auto pending = std::string {};
    return pressOn(key, menu, item, pending);
}

[[nodiscard]] core::tui::KeyEvent letter(char32_t codepoint)
{
    return core::tui::KeyEvent { .key = static_cast<core::tui::KeyCode>(codepoint), .codepoint = codepoint };
}

} // namespace

TEST_CASE("Row menu: what applies to an MR row and to a ticket row", "[orchestrator][view][work]")
{
    auto const mr = mrItem();
    auto const ticket = ticketItem();
    for (auto const& entry: RowMenuEntries)
        CHECK(menuApplies(entry.command, mr));
    CHECK(menuApplies(RowMenuCommand::StartAgent, ticket));
    CHECK_FALSE(menuApplies(RowMenuCommand::OpenMr, ticket));
    CHECK(menuApplies(RowMenuCommand::OpenTicket, ticket));
    CHECK(menuApplies(RowMenuCommand::FoldGroup, ticket));

    // A key the lookup could not find has no web page to open.
    auto keyOnly = mr;
    keyOnly.ticket.reset();
    CHECK_FALSE(menuApplies(RowMenuCommand::OpenTicket, keyOnly));
}

TEST_CASE("Row menu: the keys move, choose and close", "[orchestrator][view][work][rowmenu]")
{
    using core::tui::KeyCode;
    auto const mr = mrItem();
    auto menu = std::optional { RowMenuState { .itemKey = workItemKey(mr) } };
    CHECK_FALSE(pressOn(key(KeyCode::Up), menu, &mr).has_value());
    CHECK(menu->highlighted == 0);
    CHECK_FALSE(pressOn(key(KeyCode::Down), menu, &mr).has_value());
    CHECK(menu->highlighted == 1);
    // A letter is taken, and does nothing.
    CHECK_FALSE(pressOn(core::tui::KeyEvent { .key = KeyCode::None, .codepoint = U'x' }, menu, &mr));
    REQUIRE(menu.has_value());
    CHECK(pressOn(key(KeyCode::Enter), menu, &mr) == std::optional { RowMenuCommand::OpenMr });
    CHECK_FALSE(menu.has_value());

    SECTION("past the last entry it stays on it")
    {
        menu = RowMenuState { .itemKey = workItemKey(mr), .highlighted = RowMenuEntries.size() - 1 };
        CHECK_FALSE(pressOn(key(KeyCode::Down), menu, &mr).has_value());
        CHECK(menu->highlighted == RowMenuEntries.size() - 1);
    }
    SECTION("an entry that does not apply does nothing; esc closes")
    {
        auto const ticket = ticketItem();
        menu = RowMenuState { .itemKey = workItemKey(ticket), .highlighted = 1 };
        CHECK_FALSE(pressOn(key(KeyCode::Enter), menu, &ticket).has_value());
        REQUIRE(menu.has_value());
        CHECK_FALSE(pressOn(key(KeyCode::Escape), menu, &ticket).has_value());
        CHECK_FALSE(menu.has_value());
    }
    SECTION("a menu whose item is gone closes")
    {
        menu = RowMenuState { .itemKey = "contour/!1" };
        CHECK_FALSE(pressOn(key(KeyCode::Down), menu, nullptr).has_value());
        CHECK_FALSE(menu.has_value());
    }
}

TEST_CASE("Row menu: j, k, gg and G choose; q closes", "[orchestrator][view][work][rowmenu]")
{
    using core::tui::KeyCode;
    auto const mr = mrItem();
    auto menu = std::optional { RowMenuState { .itemKey = workItemKey(mr) } };
    auto pending = std::string {};
    CHECK_FALSE(pressOn(letter(U'j'), menu, &mr, pending).has_value());
    CHECK(menu->highlighted == 1);
    CHECK_FALSE(pressOn(letter(U'G'), menu, &mr, pending).has_value());
    CHECK(menu->highlighted == RowMenuEntries.size() - 1);
    // Past the last entry j stays on it.
    CHECK_FALSE(pressOn(letter(U'j'), menu, &mr, pending).has_value());
    CHECK(menu->highlighted == RowMenuEntries.size() - 1);
    CHECK_FALSE(pressOn(letter(U'k'), menu, &mr, pending).has_value());
    CHECK(menu->highlighted == RowMenuEntries.size() - 2);
    CHECK_FALSE(pressOn(letter(U'g'), menu, &mr, pending).has_value());
    CHECK(pending == "g");
    CHECK(menu->highlighted == RowMenuEntries.size() - 2);
    CHECK_FALSE(pressOn(letter(U'g'), menu, &mr, pending).has_value());
    CHECK(menu->highlighted == 0);
    // The arrows still move it.
    CHECK_FALSE(pressOn(key(KeyCode::Down), menu, &mr, pending).has_value());
    CHECK(menu->highlighted == 1);
    CHECK_FALSE(pressOn(letter(U'q'), menu, &mr, pending).has_value());
    CHECK_FALSE(menu.has_value());
}

TEST_CASE("Row menu: an item is found by its key", "[orchestrator][view][work]")
{
    auto const items = std::vector { mrItem(), ticketItem() };
    CHECK(workItemOf(items, "contour/PRJ-1302") == &items[1]);
    CHECK(workItemOf(items, "contour/!2375") == &items[0]);
    CHECK(workItemOf(items, "contour/!1") == nullptr);
}
