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

#include <terminal/ViInputHandler.h>

#include <utility>

namespace terminal
{

class Terminal;

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
    void execute(ViOperator op, ViMotion motion, unsigned count) override;
    void moveCursor(ViMotion motion, unsigned count) override;
    void select(TextObjectScope scope, TextObject textObject) override;
    void yank(TextObjectScope scope, TextObject textObject) override;
    void paste(unsigned count) override;

    [[nodiscard]] CellLocation translateToCellLocation(ViMotion motion, unsigned count) const noexcept;
    [[nodiscard]] CellLocationRange translateToCellRange(ViMotion motion, unsigned count) const noexcept;
    [[nodiscard]] CellLocationRange translateToCellRange(TextObjectScope scope,
                                                         TextObject textObject) const noexcept;
    [[nodiscard]] CellLocationRange expandMatchingPair(TextObjectScope scope,
                                                       char lhs,
                                                       char rhs) const noexcept;
    void executeYank(ViMotion motion, unsigned count);
    void executeYank(CellLocation from, CellLocation to);

    Terminal& terminal;

    // Cursor offset into the grid.
    CellLocation cursorPosition {};

  private:
    ViMode lastMode = ViMode::Insert;
    CursorShape lastCursorShape = CursorShape::Block;
    bool lastCursorVisible = true;
};

} // namespace terminal
