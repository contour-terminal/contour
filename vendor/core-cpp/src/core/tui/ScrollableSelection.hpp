// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>

namespace core::tui
{

/// @brief Value type managing scrollable list selection state.
///
/// Encapsulates the selected index, scroll offset, and maximum visible items
/// for popup list navigation. All navigation methods take `itemCount` as a
/// parameter since it changes dynamically (e.g., during filtering).
///
/// Used by CompletionPopup, CommandPalettePopup, and FuzzyPickerPopup
/// to share identical navigation logic without inheritance.
class ScrollableSelection
{
  public:
    /// @brief Constructs with the given maximum visible items.
    /// @param maxVisible Maximum number of items visible at once.
    explicit constexpr ScrollableSelection(size_t maxVisible = 10) noexcept:
        _maxVisible(std::max<size_t>(maxVisible, 1))
    {
    }

    // --- State Access ---

    [[nodiscard]] constexpr size_t selected() const noexcept { return _selected; }

    [[nodiscard]] constexpr size_t scrollOffset() const noexcept { return _scrollOffset; }

    [[nodiscard]] constexpr size_t maxVisible() const noexcept { return _maxVisible; }

    constexpr void setMaxVisible(size_t n) noexcept { _maxVisible = std::max<size_t>(n, 1); }

    /// @brief Sets the selected index directly and adjusts scroll.
    /// @param index The index to select (clamped to itemCount - 1).
    /// @param itemCount Total number of items.
    constexpr void select(size_t index, size_t itemCount) noexcept
    {
        _selected = (itemCount > 0) ? std::min(index, itemCount - 1) : 0;
        ensureSelectedVisible(itemCount);
    }

    /// @brief Resets selection and scroll offset to zero.
    constexpr void reset() noexcept
    {
        _selected = 0;
        _scrollOffset = 0;
    }

    // --- Navigation ---
    // All return true if the selection actually changed.

    /// @brief Selects the next item with wrap-around.
    constexpr bool selectNext(size_t itemCount) noexcept
    {
        if (itemCount == 0)
            return false;
        auto const prev = _selected;
        _selected = (_selected + 1) % itemCount;
        ensureSelectedVisible(itemCount);
        return _selected != prev;
    }

    /// @brief Selects the previous item with wrap-around.
    constexpr bool selectPrev(size_t itemCount) noexcept
    {
        if (itemCount == 0)
            return false;
        auto const prev = _selected;
        _selected = (_selected == 0) ? itemCount - 1 : _selected - 1;
        ensureSelectedVisible(itemCount);
        return _selected != prev;
    }

    /// @brief Moves selection down by one page, clamped to last item.
    constexpr bool pageDown(size_t itemCount) noexcept
    {
        if (itemCount == 0)
            return false;
        auto const prev = _selected;
        _selected = std::min(_selected + _maxVisible, itemCount - 1);
        ensureSelectedVisible(itemCount);
        return _selected != prev;
    }

    /// @brief Moves selection up by one page, clamped to first item.
    constexpr bool pageUp(size_t itemCount) noexcept
    {
        if (itemCount == 0)
            return false;
        auto const prev = _selected;
        _selected = (_selected >= _maxVisible) ? _selected - _maxVisible : 0;
        ensureSelectedVisible(itemCount);
        return _selected != prev;
    }

    /// @brief Selects the first item.
    constexpr bool selectFirst(size_t itemCount) noexcept
    {
        if (itemCount == 0)
            return false;
        auto const prev = _selected;
        _selected = 0;
        ensureSelectedVisible(itemCount);
        return _selected != prev;
    }

    /// @brief Selects the last item.
    constexpr bool selectLast(size_t itemCount) noexcept
    {
        if (itemCount == 0)
            return false;
        auto const prev = _selected;
        _selected = itemCount - 1;
        ensureSelectedVisible(itemCount);
        return _selected != prev;
    }

    /// @brief Adjusts scroll offset to keep the selected item visible.
    constexpr void ensureSelectedVisible(size_t /*itemCount*/) noexcept
    {
        if (_selected < _scrollOffset)
            _scrollOffset = _selected;
        else if (_selected >= _scrollOffset + _maxVisible)
            _scrollOffset = _selected - _maxVisible + 1;
    }

  private:
    size_t _selected = 0;
    size_t _scrollOffset = 0;
    size_t _maxVisible;
};

} // namespace core::tui
