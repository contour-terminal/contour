// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace orchestrator::view
{

/// What LazyLines reports as the number of lines while blocks are left to wrap: more than any scroll reaches,
/// so that the keys scroll on and the next draw wraps what they scrolled to.
inline constexpr auto UnknownLineCount = std::numeric_limits<std::size_t>::max() / 2;

/// Drawn lines made from blocks -- a transcript's entries, a board's lines -- in order and only on demand: a
/// block is wrapped once a line of it, or one after it, is asked for, and kept until reset(). So a draw wraps
/// what it shows and what lies above it, never what lies below.
/// @tparam Line A drawn line.
template <typename Line>
class LazyLines
{
  public:
    /// @param maxLines The most lines made; when more would follow, the last of them is @p ellipsis.
    /// @param ellipsis The line that says more were cut.
    LazyLines(std::size_t maxLines, Line ellipsis): _maxLines { maxLines }, _ellipsis { std::move(ellipsis) }
    {
    }

    /// Starts over, nothing wrapped: the blocks or the width changed.
    /// @param blocks How many blocks there are.
    void reset(std::size_t blocks)
    {
        _blocks = blocks;
        _next = 0;
        _lines.clear();
    }

    /// Wraps blocks, in order, until @p count lines are made or every block is.
    /// @param count The lines wanted.
    /// @param wrapBlock Makes block @p n's lines: `std::vector<Line>(std::size_t n)`.
    /// @return The lines made so far.
    template <typename WrapBlock>
    std::span<Line const> through(std::size_t count, WrapBlock const& wrapBlock)
    {
        while (!isComplete() && _lines.size() < count)
        {
            std::ranges::move(wrapBlock(_next++), std::back_inserter(_lines));
            ++_wrappedBlocks;
            // A block wrapped past the cap ends the lines; the last says more were cut.
            if (_lines.size() > _maxLines)
            {
                _lines.resize(_maxLines);
                _lines.back() = _ellipsis;
                _next = _blocks;
            }
        }
        return _lines;
    }

    /// @return Whether every block is wrapped, or the cap was reached.
    [[nodiscard]] bool isComplete() const noexcept { return _next >= _blocks; }

    /// @return How many lines there are once every block is wrapped; UnknownLineCount before.
    [[nodiscard]] std::size_t lineCount() const noexcept
    {
        return isComplete() ? _lines.size() : UnknownLineCount;
    }

    /// @return How many blocks were wrapped since construction, every reset() included: for tests.
    [[nodiscard]] std::size_t wrappedBlocks() const noexcept { return _wrappedBlocks; }

  private:
    std::size_t _maxLines;
    Line _ellipsis;
    std::size_t _blocks = 0;        ///< How many blocks there are.
    std::size_t _next = 0;          ///< The first block not wrapped yet.
    std::vector<Line> _lines;       ///< The lines of the blocks before @ref _next.
    std::size_t _wrappedBlocks = 0; ///< How many blocks were wrapped in all.
};

} // namespace orchestrator::view
