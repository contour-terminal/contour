// SPDX-License-Identifier: Apache-2.0
//
// The Work view's row menu: a right-click on a row, or `m`, opens it at the pointer (or under the selected
// row) with what can be done to that item; entries that do not apply to it are dimmed and do nothing.
#pragma once

#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/HitMap.hpp>

namespace orchestrator::view
{

/// What a row menu entry does.
enum class RowMenuCommand : uint8_t
{
    StartAgent = 0, ///< Open the start dialog on the item.
    OpenMr,         ///< Open its merge request in the browser.
    OpenTicket,     ///< Open its ticket in the browser.
    FoldGroup,      ///< Fold the item's group.
};

/// One entry of the row menu.
struct RowMenuEntry
{
    RowMenuCommand command; ///< What it does.
    std::string_view label; ///< What the menu calls it.
};

/// The row menu's entries, in the order it lists them.
inline constexpr auto RowMenuEntries = std::array {
    RowMenuEntry { .command = RowMenuCommand::StartAgent, .label = "Start agent…" },
    RowMenuEntry { .command = RowMenuCommand::OpenMr, .label = "Open MR" },
    RowMenuEntry { .command = RowMenuCommand::OpenTicket, .label = "Open ticket" },
    RowMenuEntry { .command = RowMenuCommand::FoldGroup, .label = "Fold group" },
};

/// An open row menu.
struct RowMenuState
{
    std::string itemKey;         ///< workItemKey() of the item it is about.
    std::size_t highlighted = 0; ///< Into RowMenuEntries.
    core::tui::Point at {};      ///< Its top-left cell on the screen, 0-based: where it was opened.
    [[nodiscard]] bool operator==(RowMenuState const&) const = default;
};

/// @param command A menu command.
/// @param item The item the menu is about.
/// @return Whether @p command applies to @p item: OpenMr needs an MR, OpenTicket a ticket URL.
[[nodiscard]] bool menuApplies(RowMenuCommand command, WorkItem const& item) noexcept;

/// @param items The Work view's items.
/// @param key workItemKey() of one of them.
/// @return The item of that key; nullptr when none is listed (any more).
[[nodiscard]] WorkItem const* workItemOf(std::span<WorkItem const> items, std::string_view key) noexcept;

/// Applies one key to the open menu: ↑/↓ move the highlight, Esc closes it, ⏎ chooses the highlighted entry
/// where it applies to the item (and closes the menu); every other key is taken and does nothing.
/// @param key A key the terminal sent.
/// @param menu The open menu; closed (reset) by Esc, by a choice, or when @p item is gone.
/// @param item The item the menu is about; nullptr: it is gone, and the menu closes.
/// @return The command chosen, if any.
[[nodiscard]] std::optional<RowMenuCommand> applyRowMenuKey(core::tui::KeyEvent const& key,
                                                            std::optional<RowMenuState>& menu,
                                                            WorkItem const* item);

/// The row menu, in a rounded box: each entry on its line, the highlighted one in the selection's style,
/// those that do not apply to the item dimmed. Records a Popup hit over its area and a MenuEntry hit per
/// entry.
class RowMenuView final: public core::tui::Component
{
  public:
    /// @param menu The open menu; nullopt while none is, when nothing is drawn. It must outlive the view.
    /// @param items The Work view's items, which the menu's item is found among; they must outlive the view.
    /// @param hits Where it records what a click on it lands on.
    RowMenuView(std::optional<RowMenuState> const& menu, std::vector<WorkItem> const& items, HitMap& hits):
        _menu { menu }, _items { items }, _hits { hits }
    {
    }

    void render(core::tui::Canvas& canvas) override;

    /// @return Its size: the widest label and the entries, in their box.
    [[nodiscard]] core::tui::Size preferredSize() const override;

  private:
    std::optional<RowMenuState> const& _menu;
    std::vector<WorkItem> const& _items;
    HitMap& _hits;
};

} // namespace orchestrator::view
