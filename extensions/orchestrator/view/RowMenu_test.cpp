// SPDX-License-Identifier: Apache-2.0
#include <core/tui/KeyCode.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/WorkItems.hpp>
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

TEST_CASE("Row menu: the keys move, choose and close", "[orchestrator][view][work]")
{
    using core::tui::KeyCode;
    auto const mr = mrItem();
    auto menu = std::optional { RowMenuState { .itemKey = workItemKey(mr) } };
    CHECK_FALSE(applyRowMenuKey(key(KeyCode::Up), menu, &mr).has_value());
    CHECK(menu->highlighted == 0);
    CHECK_FALSE(applyRowMenuKey(key(KeyCode::Down), menu, &mr).has_value());
    CHECK(menu->highlighted == 1);
    // A letter is taken, and does nothing.
    CHECK_FALSE(applyRowMenuKey(core::tui::KeyEvent { .key = KeyCode::None, .codepoint = U'x' }, menu, &mr));
    REQUIRE(menu.has_value());
    CHECK(applyRowMenuKey(key(KeyCode::Enter), menu, &mr) == std::optional { RowMenuCommand::OpenMr });
    CHECK_FALSE(menu.has_value());

    SECTION("past the last entry it stays on it")
    {
        menu = RowMenuState { .itemKey = workItemKey(mr), .highlighted = RowMenuEntries.size() - 1 };
        CHECK_FALSE(applyRowMenuKey(key(KeyCode::Down), menu, &mr).has_value());
        CHECK(menu->highlighted == RowMenuEntries.size() - 1);
    }
    SECTION("an entry that does not apply does nothing; esc closes")
    {
        auto const ticket = ticketItem();
        menu = RowMenuState { .itemKey = workItemKey(ticket), .highlighted = 1 };
        CHECK_FALSE(applyRowMenuKey(key(KeyCode::Enter), menu, &ticket).has_value());
        REQUIRE(menu.has_value());
        CHECK_FALSE(applyRowMenuKey(key(KeyCode::Escape), menu, &ticket).has_value());
        CHECK_FALSE(menu.has_value());
    }
    SECTION("a menu whose item is gone closes")
    {
        menu = RowMenuState { .itemKey = "contour/!1" };
        CHECK_FALSE(applyRowMenuKey(key(KeyCode::Down), menu, nullptr).has_value());
        CHECK_FALSE(menu.has_value());
    }
}

TEST_CASE("Row menu: an item is found by its key", "[orchestrator][view][work]")
{
    auto const items = std::vector { mrItem(), ticketItem() };
    CHECK(workItemOf(items, "contour/PRJ-1302") == &items[1]);
    CHECK(workItemOf(items, "contour/!2375") == &items[0]);
    CHECK(workItemOf(items, "contour/!1") == nullptr);
}
