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

#include <terminal/Commands.h>

#include <list>
#include <cstdint>

namespace terminal {

class Screen;

class Debugger : public CommandVisitor {
  private:
    Screen& screen_;
    std::list<Command> queuedCommands_{};
    int64_t pointer_ = 0;

  public:
    explicit Debugger(Screen& _screen)
        : screen_{_screen}
    {}

    Screen& screen() noexcept { return screen_; }

    /// @returns the next command to be executed or nullptr if none pending
    Command const* nextCommand() const noexcept;

    /// Executes one command.
    void step();

    /// @returns ID of next instruction to be executed.
    ///
    /// The ID is a monotonically increasing number, starting with 1 at construction.
    int64_t pointer() const noexcept { return pointer_; }

    /// Applies all queued commands.
    void flush();

    // {{{ CommandExecutor overrides
    void visit(AppendChar const& v) override { enqueue(v); };
    void visit(ApplicationKeypadMode const& v) override { enqueue(v); };
    void visit(BackIndex const& v) override { enqueue(v); };
    void visit(Backspace const& v) override { enqueue(v); };
    void visit(Bell const& v) override { enqueue(v); };
    void visit(ChangeIconTitle const& v) override { enqueue(v); };
    void visit(ChangeWindowTitle const& v) override { enqueue(v); };
    void visit(ClearLine const& v) override { enqueue(v); };
    void visit(ClearScreen const& v) override { enqueue(v); };
    void visit(ClearScrollbackBuffer const& v) override { enqueue(v); };
    void visit(ClearToBeginOfLine const& v) override { enqueue(v); };
    void visit(ClearToBeginOfScreen const& v) override { enqueue(v); };
    void visit(ClearToEndOfLine const& v) override { enqueue(v); };
    void visit(ClearToEndOfScreen const& v) override { enqueue(v); };
    void visit(CopyToClipboard const& v) override { enqueue(v); };
    void visit(CursorBackwardTab const& v) override { enqueue(v); };
    void visit(CursorNextLine const& v) override { enqueue(v); };
    void visit(CursorPreviousLine const& v) override { enqueue(v); };
    void visit(DeleteCharacters const& v) override { enqueue(v); };
    void visit(DeleteColumns const& v) override { enqueue(v); };
    void visit(DeleteLines const& v) override { enqueue(v); };
    void visit(DesignateCharset const& v) override { enqueue(v); };
    void visit(DeviceStatusReport const& v) override { enqueue(v); };
    void visit(DumpState const& v) override { enqueue(v); };
    void visit(EraseCharacters const& v) override { enqueue(v); };
    void visit(ForwardIndex const& v) override { enqueue(v); };
    void visit(FullReset const& v) override { enqueue(v); };
    void visit(HorizontalPositionAbsolute const& v) override { enqueue(v); };
    void visit(HorizontalPositionRelative const& v) override { enqueue(v); };
    void visit(HorizontalTabClear const& v) override { enqueue(v); };
    void visit(HorizontalTabSet const& v) override { enqueue(v); };
    void visit(Hyperlink const& v) override { enqueue(v); };
    void visit(Index const& v) override { enqueue(v); };
    void visit(InsertCharacters const& v) override { enqueue(v); };
    void visit(InsertColumns const& v) override { enqueue(v); };
    void visit(InsertLines const& v) override { enqueue(v); };
    void visit(Linefeed const& v) override { enqueue(v); };
    void visit(MoveCursorBackward const& v) override { enqueue(v); };
    void visit(MoveCursorDown const& v) override { enqueue(v); };
    void visit(MoveCursorForward const& v) override { enqueue(v); };
    void visit(MoveCursorTo const& v) override { enqueue(v); };
    void visit(MoveCursorToBeginOfLine const& v) override { enqueue(v); };
    void visit(MoveCursorToColumn const& v) override { enqueue(v); };
    void visit(MoveCursorToLine const& v) override { enqueue(v); };
    void visit(MoveCursorToNextTab const& v) override { enqueue(v); };
    void visit(MoveCursorUp const& v) override { enqueue(v); };
    void visit(Notify const& v) override { enqueue(v); };
    void visit(ReportCursorPosition const& v) override { enqueue(v); };
    void visit(ReportExtendedCursorPosition const& v) override { enqueue(v); };
    void visit(RequestDynamicColor const& v) override { enqueue(v); };
    void visit(RequestMode const& v) override { enqueue(v); };
    void visit(RequestStatusString const& v) override { enqueue(v); };
    void visit(RequestTabStops const& v) override { enqueue(v); };
    void visit(ResetDynamicColor const& v) override { enqueue(v); };
    void visit(ResizeWindow const& v) override { enqueue(v); };
    void visit(RestoreCursor const& v) override { enqueue(v); };
    void visit(RestoreWindowTitle const& v) override { enqueue(v); };
    void visit(ReverseIndex const& v) override { enqueue(v); };
    void visit(SaveCursor const& v) override { enqueue(v); };
    void visit(SaveWindowTitle const& v) override { enqueue(v); };
    void visit(ScreenAlignmentPattern const& v) override { enqueue(v); };
    void visit(ScrollDown const& v) override { enqueue(v); };
    void visit(ScrollUp const& v) override { enqueue(v); };
    void visit(SelectConformanceLevel const& v) override { enqueue(v); };
    void visit(SendDeviceAttributes const& v) override { enqueue(v); };
    void visit(SendMouseEvents const& v) override { enqueue(v); };
    void visit(SendTerminalId const& v) override { enqueue(v); };
    void visit(SetBackgroundColor const& v) override { enqueue(v); };
    void visit(SetCursorStyle const& v) override { enqueue(v); };
    void visit(SetDynamicColor const& v) override { enqueue(v); };
    void visit(SetForegroundColor const& v) override { enqueue(v); };
    void visit(SetGraphicsRendition const& v) override { enqueue(v); };
    void visit(SetLeftRightMargin const& v) override { enqueue(v); };
    void visit(SetMark const& v) override { enqueue(v); };
    void visit(SetMode const& v) override { enqueue(v); };
    void visit(SetTopBottomMargin const& v) override { enqueue(v); };
    void visit(SetUnderlineColor const& v) override { enqueue(v); };
    void visit(SingleShiftSelect const& v) override { enqueue(v); };
    void visit(SoftTerminalReset const& v) override { enqueue(v); };
    void visit(UnknownCommand const& v) override { enqueue(v); };
    // }}}

  private:
    void enqueue(Command&& _cmd) { queuedCommands_.emplace_back(std::move(_cmd)); }
};

} // end namespace
