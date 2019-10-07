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
  protected:
    explicit Selector(terminal::Coordinate const& _from);

  public:
    using Renderer = terminal::Screen::Renderer;

    virtual ~Selector() = default;

    bool active() const noexcept { return active_; }

    // TODO: maybe make Coordinate's members signed, whereas negative row values represent saved (history) lines?

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

    // Returns the text contents from the selected area from @p _source.
    virtual void copy(terminal::Terminal const& _source, size_t _scrollOffset, Renderer _render) const = 0;

  protected:
    terminal::Coordinate from_{};
    terminal::Coordinate to_{};

  private:
    bool active_{true};
};

/// Linearly selects lines. Inner lines are always full, first and last line can be partial.
///
/// Usually triggered by single left-click.
class LinearSelector : public Selector {
  public:
    explicit LinearSelector(terminal::Coordinate const& _start) : Selector{_start} {}

    void copy(terminal::Terminal const& _source, size_t _scrollOffset, Renderer _render) const override;
};

/// Selects full lines.
///
/// Usually triggered by double left-click.
class FullLineSelector : public Selector {
  public:
    void copy(terminal::Terminal const& _source, size_t _scrollOffset, Renderer _render) const override;
};

/// Selects a rectangular block of equal-width partial lines.
///
/// Usually triggered by Alt + single left-click.
class BlockSelector : public Selector {
  public:
    void copy(terminal::Terminal const& _source, size_t _scrollOffset, Renderer _render) const override;
};
