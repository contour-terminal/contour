// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <algorithm>
#include <iterator>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace vtbackend
{

/// The horizontal tab stops of a terminal.
///
/// A stop belongs to a column, not to the page: the stops are there before the page is as wide as
/// they reach, and stay when it becomes narrower. A terminal starts with one every DefaultInterval
/// columns, and HTS, TBC and a reset change them from there.
///
/// That initial state is kept as a rule rather than as a list -- "a stop every DefaultInterval
/// columns from here on" -- so it holds for columns no page has shown yet. Setting or clearing a
/// stop writes the rule out as far as that column; TBC 3 retires it, which is what tells a terminal
/// without any stop from one that has never been given any.
class TabStops
{
  public:
    /// The distance between the stops a terminal starts with.
    static constexpr auto DefaultInterval = ColumnCount(8);

    /// Places a stop on @p column (HTS). A column that has one keeps it.
    /// @param column The column, counted from the page's first.
    void set(ColumnOffset column)
    {
        writeOutDefaults(column + 1);
        if (auto const position = std::ranges::lower_bound(_stops, column);
            position == _stops.end() || *position != column)
            _stops.insert(position, column);
    }

    /// Removes the stop on @p column (TBC 0), if there is one.
    /// @param column The column, counted from the page's first.
    void clear(ColumnOffset column)
    {
        writeOutDefaults(column + 1);
        if (auto const position = std::ranges::lower_bound(_stops, column);
            position != _stops.end() && *position == column)
            _stops.erase(position);
    }

    /// Removes every stop (TBC 3), those of columns beyond the page included.
    void clearAll() noexcept
    {
        _stops.clear();
        _defaultsFrom.reset();
    }

    /// Returns to the stops a terminal starts with.
    void reset() noexcept { *this = TabStops {}; }

    /// @return true if these are the stops a terminal starts with, none set or cleared since.
    [[nodiscard]] bool isInitial() const noexcept
    {
        return _stops.empty() && _defaultsFrom == ColumnOffset(0);
    }

    /// @param column The column to look right of.
    /// @return The first stop right of @p column, or nothing when there is none.
    [[nodiscard]] std::optional<ColumnOffset> nextAfter(ColumnOffset column) const noexcept
    {
        if (auto const position = std::ranges::upper_bound(_stops, column); position != _stops.end())
            return *position;
        if (!_defaultsFrom)
            return std::nullopt;
        return firstDefaultStopFrom(std::max(*_defaultsFrom, column + 1));
    }

    /// @param column The column to look left of.
    /// @return The last stop left of @p column, or nothing when there is none.
    [[nodiscard]] std::optional<ColumnOffset> previousBefore(ColumnOffset column) const noexcept
    {
        if (column <= ColumnOffset(0))
            return std::nullopt;
        // The stops written out all lie left of where the rule takes over, so the rule is asked first.
        if (auto const last = lastDefaultStopUpTo(column - 1); isDefaultStop(last))
            return last;
        if (auto const position = std::ranges::lower_bound(_stops, column); position != _stops.begin())
            return *std::ranges::prev(position);
        return std::nullopt;
    }

    /// Lists the stops of a page.
    /// @param columns The page's width.
    /// @return The stops on the first @p columns columns, ascending.
    [[nodiscard]] std::vector<ColumnOffset> within(ColumnCount columns) const
    {
        auto const end = boxed_cast<ColumnOffset>(columns);
        auto stops = std::vector<ColumnOffset> {};
        std::ranges::copy(_stops | std::views::take_while([&](ColumnOffset stop) { return stop < end; }),
                          std::back_inserter(stops));
        if (_defaultsFrom)
            appendDefaultStops(stops, *_defaultsFrom, end);
        return stops;
    }

    /// Takes the stops a list names, and none besides.
    /// @param stops The stops, ascending and without duplicates.
    /// @return Those stops, with none on any column the list does not name.
    [[nodiscard]] static TabStops only(std::vector<ColumnOffset> stops) noexcept
    {
        auto result = TabStops {};
        result._stops = std::move(stops);
        result._defaultsFrom.reset();
        return result;
    }

    [[nodiscard]] bool operator==(TabStops const&) const noexcept = default;

  private:
    /// @return true if the rule, where it applies, puts a stop on @p column.
    [[nodiscard]] bool isDefaultStop(ColumnOffset column) const noexcept
    {
        return _defaultsFrom && column >= *_defaultsFrom && *column % *DefaultInterval == 0;
    }

    /// @return The first column at or right of @p column that is a multiple of DefaultInterval.
    [[nodiscard]] static ColumnOffset firstDefaultStopFrom(ColumnOffset column) noexcept
    {
        auto const interval = *DefaultInterval;
        return ColumnOffset(((*column + interval - 1) / interval) * interval);
    }

    /// @return The last column at or left of @p column that is a multiple of DefaultInterval.
    [[nodiscard]] static ColumnOffset lastDefaultStopUpTo(ColumnOffset column) noexcept
    {
        return ColumnOffset((*column / *DefaultInterval) * *DefaultInterval);
    }

    /// Appends the stops the rule puts on [@p from, @p end) to @p stops.
    static void appendDefaultStops(std::vector<ColumnOffset>& stops, ColumnOffset from, ColumnOffset end)
    {
        auto const first = *firstDefaultStopFrom(from);
        if (first >= *end)
            return;
        std::ranges::copy(std::views::iota(0, ((*end - first - 1) / *DefaultInterval) + 1)
                              | std::views::transform([&](int index) {
                                    return ColumnOffset(first + (index * *DefaultInterval));
                                }),
                          std::back_inserter(stops));
    }

    /// Turns the rule into listed stops for every column left of @p end, so that one of them can be
    /// set or cleared on its own. The rule then starts at @p end.
    void writeOutDefaults(ColumnOffset end)
    {
        if (!_defaultsFrom || *_defaultsFrom >= end)
            return;
        appendDefaultStops(_stops, *_defaultsFrom, end);
        _defaultsFrom = end;
    }

    /// The stops written out one by one, ascending; all of them left of _defaultsFrom while that is set.
    std::vector<ColumnOffset> _stops;
    /// The column from which there is a stop every DefaultInterval columns; nothing once TBC 3 has
    /// retired that rule.
    std::optional<ColumnOffset> _defaultsFrom { ColumnOffset(0) };
};

} // namespace vtbackend
