// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/vi/JumpHistory.hpp>
#include <vtbackend/input/vi/ViInputHandler.hpp>
#include <vtbackend/shell/Folding.hpp>

#include <gsl/pointers>

#include <list>
#include <optional>

namespace vtbackend
{

class Terminal;

enum class JumpOver : uint8_t
{
    Yes,
    No
};

/**
 * Implements the Vi commands for a Terminal as emitted by ViInputHandler.
 */
class ViCommands: public ViInputHandler::Executor
{
  public:
    explicit ViCommands(Terminal& theTerminal);

    void scrollViewport(ScrollOffset delta) override;
    void modeChanged(ViMode mode) override;
    void reverseSearchCurrentWord() override;
    void toggleLineMark() override;
    void searchCurrentWord() override;
    void enterHintMode(HintAction action) override;
    void execute(ViOperator op, ViMotion motion, unsigned count, char32_t lastChar = U'\0') override;
    void moveCursor(ViMotion motion, unsigned count, char32_t lastChar = U'\0') override;
    void select(TextObjectScope scope, TextObject textObject) override;
    void yank(TextObjectScope scope, TextObject textObject) override;
    void yank(ViMotion motion) override;
    void open(TextObjectScope scope, TextObject textObject) override;
    void paste(unsigned count, bool stripped) override;

    void searchStart() override;
    void searchDone() override;
    void searchCancel() override;
    void updateSearchTerm(std::u32string const& text) override;

    bool jumpToNextMatch(unsigned count);
    bool jumpToPreviousMatch(unsigned count);

    void moveCursorTo(CellLocation position);

    [[nodiscard]] CellLocation translateToCellLocationAndRecord(ViMotion motion, unsigned count) noexcept;
    [[nodiscard]] CellLocationRange translateToCellRange(ViMotion motion, unsigned count) noexcept;
    [[nodiscard]] CellLocationRange translateToCellRange(TextObjectScope scope,
                                                         TextObject textObject) const noexcept;
    [[nodiscard]] CellLocation prev(CellLocation location) const noexcept;
    [[nodiscard]] CellLocation next(CellLocation location) const noexcept;
    [[nodiscard]] CellLocation findMatchingPairFrom(CellLocation location) const noexcept;
    [[nodiscard]] CellLocation findMatchingPairLeft(char32_t left,
                                                    char32_t right,
                                                    int initialDepth) const noexcept;
    [[nodiscard]] CellLocation findMatchingPairRight(char32_t left,
                                                     char32_t right,
                                                     int initialDepth) const noexcept;
    [[nodiscard]] CellLocationRange expandMatchingPair(TextObjectScope scope,
                                                       char left,
                                                       char right) const noexcept;
    [[nodiscard]] CellLocation findBeginOfWordAt(CellLocation location, JumpOver jumpOver) const noexcept;
    [[nodiscard]] CellLocation findEndOfWordAt(CellLocation location, JumpOver jumpOver) const noexcept;
    /// One VISIBLE line from @p line in @p direction, or @p line itself at the edge of the grid.
    ///
    /// The step every line-walking motion takes, so none of them can walk into a collapsed block.
    /// Returning @p line unchanged at the edge is what lets those loops end on "stopped moving"
    /// rather than each restating a bound the fold projection has already accounted for.
    [[nodiscard]] LineOffset stepVisible(LineOffset line, VerticalDirection direction) const noexcept;

    /// Moves @p line one VISIBLE line in @p direction, reporting whether it moved at all.
    ///
    /// The termination condition every line-scanning motion below shares: the walk ends when the step
    /// stalls, which is the grid's edge. One helper rather than the same three lines at each of them.
    ///
    /// @param line The line to advance, updated in place when it moves.
    /// @param direction Which way to step.
    /// @return Whether @p line changed.
    [[nodiscard]] bool tryStepVisible(LineOffset& line, VerticalDirection direction) const noexcept;

    /// Resolves a jump target that lands inside a collapsed fold, per Settings::foldJumpBehavior.
    ///
    /// Not const: FoldJumpBehavior::Expand opens the block, which is the point of it.
    ///
    /// @param position The target a jump named.
    /// @return @p position when it is visible or was revealed, the nearest visible line otherwise.
    [[nodiscard]] CellLocation revealOrSnap(CellLocation position);

    [[nodiscard]] CellLocation globalCharUp(CellLocation location, char ch, unsigned count) const noexcept;
    [[nodiscard]] CellLocation globalCharDown(CellLocation location, char ch, unsigned count) const noexcept;
    [[nodiscard]] std::optional<CellLocation> toCharRight(CellLocation startPosition) const noexcept;
    [[nodiscard]] std::optional<CellLocation> toCharLeft(CellLocation startPosition) const noexcept;
    [[nodiscard]] std::optional<CellLocation> toCharRight(unsigned count) const noexcept;
    [[nodiscard]] std::optional<CellLocation> toCharLeft(unsigned count) const noexcept;
    void executeYank(ViMotion motion, unsigned count);
    void executeYank(CellLocation from, CellLocation to);
    void executeOpen(ViMotion motion, unsigned count);
    void executeOpen(CellLocation from, CellLocation to);

    std::string extractTextAndHighlightRange(CellLocation from, CellLocation to);

    /// Snaps the input location to the correct cell location if the input location is part of a wide char
    /// cell but not precisely the beginning cell location.
    [[nodiscard]] CellLocation snapToCell(CellLocation location) const noexcept;

    /// Snaps the input location to the cell right next to it iff the current cell does not contain
    /// any codepoints.
    [[nodiscard]] CellLocation snapToCellRight(CellLocation location) const noexcept;

    [[nodiscard]] bool compareCellTextAt(CellLocation position, char32_t codepoint) const noexcept;
    void addLineOffsetToJumpHistory(LineOffset offset) { _jumpHistory.addOffset(offset); }
    // Cursor offset into the grid.
    CellLocation cursorPosition {};

  private:
    gsl::not_null<Terminal*> _terminal;
    ViMode _lastMode = ViMode::Insert;
    CursorShape _lastCursorShape = CursorShape::Block;
    mutable char32_t _lastChar = U'\0';
    std::optional<ViMotion> _lastCharMotion = std::nullopt;
    bool _lastCursorVisible = true;
    JumpHistory _jumpHistory;
};

} // namespace vtbackend
