// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/StateDecodeError.hpp>

#include <expected>
#include <vector>

namespace vtbackend
{

/// What a JumpHistory holds, as plain data.
struct JumpHistoryState
{
    std::vector<CellLocation> history {}; ///< The marked positions, oldest first.
    uint64_t current = 0;                 ///< Index of the position the history is at.
    LineOffset offsetSinceLastJump {};    ///< Scrolling not yet applied to the positions.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.history);
        visitor(2, self.current);
        visitor(3, self.offsetSinceLastJump);
    }
};

class JumpHistory
{
  public:
    /// @return The positions and the pending offset.
    [[nodiscard]] JumpHistoryState captureState() const
    {
        return JumpHistoryState { .history = _history,
                                  .current = _current,
                                  .offsetSinceLastJump = _offsetSinceLastJump };
    }

    /// @return Nothing, or InvalidValue when the index @p state names is past the end of its history.
    [[nodiscard]] static std::expected<void, StateDecodeError> validateState(JumpHistoryState const& state)
    {
        if (state.current > state.history.size())
            return std::unexpected { StateDecodeError::InvalidValue };
        return {};
    }

    /// Replaces the history with @p state.
    /// @return Nothing, or what validateState() refuses; the history is then unchanged.
    [[nodiscard]] std::expected<void, StateDecodeError> restoreState(JumpHistoryState const& state)
    {
        if (auto const valid = validateState(state); !valid)
            return valid;
        _history = state.history;
        _current = static_cast<size_t>(state.current);
        _offsetSinceLastJump = state.offsetSinceLastJump;
        return {};
    }

    template <typename T>
    void add(T&& cell)
    {
        applyOffset();
        _history.push_back(std::forward<T>(cell));
    }
    CellLocation jumpToLast(CellLocation current);
    CellLocation jumpToMarkBackward(CellLocation current);
    CellLocation jumpToMarkForward(CellLocation current);
    void addOffset(LineOffset offset) { _offsetSinceLastJump += offset; }

  private:
    std::vector<CellLocation> _history;
    size_t _current = 0;
    LineOffset _offsetSinceLastJump { 0 };
    void applyOffset()
    {
        if (unbox(_offsetSinceLastJump) == 0)
            return;
        for (auto& cell: _history)
        {
            // minus since we are going in the history
            cell.line -= _offsetSinceLastJump;
        }
        _offsetSinceLastJump = LineOffset { 0 };
    }
};
} // namespace vtbackend
