/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <terminal/InputGenerator.h>
#include <terminal/Screen.h> // Coordinate
#include <terminal/Terminal.h>

#include <functional>
#include <vector>
#include <utility>

/**
 * Selector API.
 *
 * A Selector can select a range of text. The range can be linear with partial start/end lines, or full lines,
 * or a block based selector, that is capable of selecting all lines partially.
 *
 * The Selector operates on the Screen by accumulating a scrolling offset, that determines
 * the view port of that Screen.
 *
 * When the screen is being modified while selecting text, the selection regions must be preserved,
 * that is, when the selection start is inside the screen, then new lines are added, which causes the screen
 * to move the screen contents up, then also the selection's begin (and extend) is being moved up.
 *
 * This is achieved by using absolute coordinates from the top history line.
 */
class Selector {
  public:
    enum class State {
        /// Inactive, but waiting for the selection to be started (by moving the cursor).
        Waiting,
        /// Active, with selection in progress.
        InProgress,
        /// Inactive, with selection available.
        Complete,
    };

    using Renderer = terminal::Screen::Renderer;

    Selector(terminal::WindowSize const& _viewport, terminal::Coordinate const& _from);

    /// Tests whether the a selection is currently in progress.
    constexpr State state() const noexcept { return state_; }

    /// Starts or restarts a selection.
    ///
    /// @param _from determines the absolute coordinate into the Screen
    void restart(terminal::Coordinate const& _from);

    /// @todo Should be able to handle negative (or 0) and overflow coordinates,
    ///       which should potentially adjust the screen's view (aka. modifying scrolling offset).
    ///
    /// @retval true TerminalView requires scrolling offset adjustments.
    /// @retval false TerminalView's scrolling offset does not need adjustments.
    bool extend(terminal::Coordinate const& _to);

    /// Marks the selection as completed.
    void stop();

    /// When screen lines are sliced into or out of the saved lines buffer, this call will update
    /// the selection accordingly.
    void slice(int _offset);

    struct Range {
        terminal::cursor_pos_t line;
        terminal::cursor_pos_t fromColumn;
        terminal::cursor_pos_t toColumn;

        constexpr terminal::cursor_pos_t length() const noexcept { return toColumn - fromColumn + 1; }
    };

    constexpr terminal::WindowSize const& viewport() const noexcept { return viewport_; }
    constexpr terminal::Coordinate const& from() const noexcept { return from_; }
    constexpr terminal::Coordinate const& to() const noexcept { return to_; }

  private:
    terminal::WindowSize const viewport_;
    terminal::Coordinate from_{};
    terminal::Coordinate to_{};
    State state_{State::Waiting};
};

/// Constructs a vector of ranges for a linear selection strategy.
std::vector<Selector::Range> linear(Selector const& _selector);

/// Constructs a vector of ranges for a full-line selection strategy.
std::vector<Selector::Range> fullLine(Selector const& _selector);

/// Constructs a vector of ranges for a rectangular selection strategy.
std::vector<Selector::Range> block(Selector const& _selector);

/// Renders (extracts) the selected ranges from given @p _source and passes
/// each cell linearly into @p _render.
void copy(std::vector<Selector::Range> const& _ranges,
          terminal::Terminal /*TODO: should be Screen*/ const& _source,
          terminal::Screen::Renderer _render);
