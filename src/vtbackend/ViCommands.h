/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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

#include <vtbackend/ViInputHandler.h>

#include <optional>
#include <utility>

namespace terminal
{

class Terminal;

enum class jump_over
{
    Yes,
    No
};

/**
 * Implements the Vi commands for a Terminal as emitted by ViInputHandler.
 */
class vi_commands: public vi_input_handler::executor
{
  public:
    explicit vi_commands(Terminal& theTerminal);

    void scrollViewport(scroll_offset delta) override;
    void modeChanged(vi_mode mode) override;
    void reverseSearchCurrentWord() override;
    void toggleLineMark() override;
    void searchCurrentWord() override;
    void execute(vi_operator op, vi_motion motion, unsigned count, char32_t lastChar = U'\0') override;
    void moveCursor(vi_motion motion, unsigned count, char32_t lastChar = U'\0') override;
    void select(text_object_scope scope, text_object textObject) override;
    void yank(text_object_scope scope, text_object textObject) override;
    void yank(vi_motion motion) override;
    void paste(unsigned count, bool stripped) override;

    void searchStart() override;
    void searchDone() override;
    void searchCancel() override;
    void updateSearchTerm(std::u32string const& text) override;
    bool jumpToNextMatch(unsigned count);
    bool jumpToPreviousMatch(unsigned count);

    void moveCursorTo(cell_location position);

    [[nodiscard]] cell_location translateToCellLocation(vi_motion motion, unsigned count) const noexcept;
    [[nodiscard]] cell_location_range translateToCellRange(vi_motion motion, unsigned count) const noexcept;
    [[nodiscard]] cell_location_range translateToCellRange(text_object_scope scope,
                                                           text_object textObject) const noexcept;
    [[nodiscard]] cell_location prev(cell_location location) const noexcept;
    [[nodiscard]] cell_location next(cell_location location) const noexcept;
    [[nodiscard]] cell_location findMatchingPairFrom(cell_location location) const noexcept;
    [[nodiscard]] cell_location findMatchingPairLeft(char left, char right, int initialDepth) const noexcept;
    [[nodiscard]] cell_location findMatchingPairRight(char left, char right, int initialDepth) const noexcept;
    [[nodiscard]] cell_location_range expandMatchingPair(text_object_scope scope,
                                                         char left,
                                                         char right) const noexcept;
    [[nodiscard]] cell_location findBeginOfWordAt(cell_location location, jump_over jumpOver) const noexcept;
    [[nodiscard]] cell_location findEndOfWordAt(cell_location location, jump_over jumpOver) const noexcept;
    [[nodiscard]] cell_location globalCharUp(cell_location location, char ch, unsigned count) const noexcept;
    [[nodiscard]] cell_location globalCharDown(cell_location location,
                                               char ch,
                                               unsigned count) const noexcept;
    [[nodiscard]] std::optional<cell_location> toCharRight(cell_location startPosition) const noexcept;
    [[nodiscard]] std::optional<cell_location> toCharLeft(cell_location startPosition) const noexcept;
    [[nodiscard]] std::optional<cell_location> toCharRight(unsigned count) const noexcept;
    [[nodiscard]] std::optional<cell_location> toCharLeft(unsigned count) const noexcept;
    void executeYank(vi_motion motion, unsigned count);
    void executeYank(cell_location from, cell_location to);

    /// Snaps the input location to the correct cell location if the input location is part of a wide char
    /// cell but not precisely the beginning cell location.
    [[nodiscard]] cell_location snapToCell(cell_location location) const noexcept;

    /// Snaps the input location to the cell right next to it iff the current cell does not contain
    /// any codepoints.
    [[nodiscard]] cell_location snapToCellRight(cell_location location) const noexcept;

    [[nodiscard]] bool compareCellTextAt(cell_location position, char codepoint) const noexcept;

    // Cursor offset into the grid.
    cell_location cursorPosition {};

  private:
    Terminal& _terminal;
    vi_mode _lastMode = vi_mode::Insert;
    cursor_shape _lastCursorShape = cursor_shape::Block;
    mutable char32_t _lastChar = U'\0';
    std::optional<vi_motion> _lastCharMotion = std::nullopt;
    bool _lastCursorVisible = true;
};

} // namespace terminal
