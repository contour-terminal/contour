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
#include <fmt/format.h>
#include <terminal/Commands.h>
#include <terminal/Util.h>
#include <sstream>

using namespace std;
using fmt::format;

namespace terminal {

string to_string(GraphicsRendition s)
{
    switch (s)
    {
        case GraphicsRendition::Reset:
            return "Reset";
        case GraphicsRendition::Bold:
            return "Bold";
        case GraphicsRendition::Faint:
            return "Faint";
        case GraphicsRendition::Italic:
            return "Italic";
        case GraphicsRendition::Underline:
            return "Underline";
        case GraphicsRendition::Blinking:
            return "Blinking";
        case GraphicsRendition::Inverse:
            return "Inverse";
        case GraphicsRendition::Hidden:
            return "Hidden";
        case GraphicsRendition::CrossedOut:
            return "CrossedOut";
        case GraphicsRendition::DoublyUnderlined:
            return "DoublyUnderlined";
        case GraphicsRendition::Normal:
            return "Normal";
        case GraphicsRendition::NoItalic:
            return "NoItalic";
        case GraphicsRendition::NoUnderline:
            return "NoUnderline";
        case GraphicsRendition::NoBlinking:
            return "NoBlinking";
        case GraphicsRendition::NoInverse:
            return "NoInverse";
        case GraphicsRendition::NoHidden:
            return "NoHidden";
        case GraphicsRendition::NoCrossedOut:
            return "NoCrossedOut";
    }
    return "?";
}

string to_string(Mode m)
{
    switch (m)
    {
        case Mode::KeyboardAction:
            return "KeyboardAction";
        case Mode::Insert:
            return "Insert";
        case Mode::SendReceive:
            return "SendReceive";
        case Mode::AutomaticNewLine:
            return "AutomaticNewLine";
        case Mode::UseApplicationCursorKeys:
            return "UseApplicationCursorKeys";
        case Mode::DesignateCharsetUSASCII:
            return "DesignateCharsetUSASCII";
        case Mode::Columns132:
            return "Columns132";
        case Mode::SmoothScroll:
            return "SmoothScroll";
        case Mode::ReverseVideo:
            return "ReverseVideo";
        case Mode::Origin:
            return "Origin";
        case Mode::AutoWrap:
            return "AutoWrap";
        case Mode::PrinterExtend:
            return "PrinterExtend";
        case Mode::LeftRightMargin:
            return "LeftRightMargin";
        case Mode::ShowToolbar:
            return "ShowToolbar";
        case Mode::BlinkingCursor:
            return "BlinkingCursor";
        case Mode::VisibleCursor:
            return "VisibleCursor";
        case Mode::ShowScrollbar:
            return "ShowScrollbar";
        case Mode::UseAlternateScreen:
            return "UseAlternateScreen";
        case Mode::BracketedPaste:
            return "BracketedPaste";
    }
    return "?";
}

string to_string(CharsetTable i)
{
    switch (i)
    {
        case CharsetTable::G0:
            return "G0";
        case CharsetTable::G1:
            return "G1";
        case CharsetTable::G2:
            return "G2";
        case CharsetTable::G3:
            return "G3";
    }
    return "G" + std::to_string(static_cast<int>(i));
}

string to_string(Charset charset)
{
    switch (charset)
    {
        case Charset::Special:
            return "Special";
        case Charset::UK:
            return "UK";
        case Charset::USASCII:
            return "USASCII";
        case Charset::German:
            return "German";
    }
    return fmt::format("<?Charset:{}>", static_cast<unsigned>(charset));
}

unsigned to_code(MouseProtocol protocol)
{
    return static_cast<unsigned>(protocol);
}

string to_string(MouseProtocol protocol)
{
    switch (protocol)
    {
        case MouseProtocol::X10:
            return "X10";
        case MouseProtocol::VT200:
            return "VT200";
        case MouseProtocol::VT200_Highlight:
            return "VT200_Highlight";
        case MouseProtocol::ButtonEvent:
            return "ButtonEvent";
        case MouseProtocol::AnyEvent:
            return "AnyEvent";
        case MouseProtocol::FocusEvent:
            return "FocusEvent";
        case MouseProtocol::Extended:
            return "Extended";
        case MouseProtocol::SGR:
            return "SGR";
        case MouseProtocol::URXVT:
            return "URXVT";
        case MouseProtocol::AlternateScroll:
            return "AlternateScroll";
    }

    return fmt::format("<?MouseProtocol:{}>", static_cast<unsigned>(protocol));
}

class MnemonicBuilder {
  public:
    MnemonicBuilder(bool _withParameters, bool _withComment) :
        withParameters_{ _withParameters },
        withComment_{ _withComment }
        {}

    vector<string> build(vector<Command> const& _commands)
    {
        for (Command const& command : _commands)
            visit(*this, command);

        return result();
    }

    vector<string> result()
    {
        flushPendingText();
        return result_;
    }

    void operator()(Bell const& v) { build("\\a"); }
    void operator()(FullReset const& v) { build("RIS", "Reset to Initial state (hard reset)"); }
    void operator()(Linefeed const& v) { build("\\n"); }
    void operator()(Backspace const& v) { build("\\b"); }
    void operator()(DeviceStatusReport const& v) {} // TODO
    void operator()(ReportCursorPosition const& v) { build("CPR", "Report cursor position"); }
    void operator()(ReportExtendedCursorPosition const& v) { build("DECXCPR", "Report cursor position (extended)."); }
    void operator()(SendDeviceAttributes const& v) {} // TODO
    void operator()(SendTerminalId const& v) {} // TODO
    void operator()(ClearToEndOfScreen const& v) { build("ED", "Clear to end of screen", 0); }
    void operator()(ClearToBeginOfScreen const& v) { build("ED", "Clear to begin of screen", 1); }
    void operator()(ClearScreen const& v) { build("ED", "Clear screen", 2); }
    void operator()(ClearScrollbackBuffer const& v) { build("ED", "Clear scrollback buffer", 3); }
    void operator()(EraseCharacters const& v) { build("ECH", "Erase characters", v.n); }
    void operator()(ScrollUp const& v) { build("SU", "Scroll up", v.n); }
    void operator()(ScrollDown const& v) { build("SD", "Scroll down", v.n); }
    void operator()(ClearToEndOfLine const& v) { build("EL", "Clear to end of line", 0); }
    void operator()(ClearToBeginOfLine const& v) { build("EL", "Clear to begin of line", 1); }
    void operator()(ClearLine const& v)  { build("EL", "Clear line", 2); }
    void operator()(CursorNextLine const& v) { build("CNL", "Cursor Next Line", v.n); }
    void operator()(CursorPreviousLine const& v) { build("CNL", "Cursor Previous Line", v.n); }
    void operator()(InsertCharacters const& v) { build("ICH", "Insert Characters", v.n); }
    void operator()(InsertColumns const& v) { build("DECIC", "Insert Columns", v.n); }
    void operator()(InsertLines const& v) { build("IL", "Insert Lines", v.n); }
    void operator()(DeleteLines const& v) { build("DL", "Delete Lines", v.n); }
    void operator()(DeleteCharacters const& v) { build("DCH", "Delete characters", v.n); }
    void operator()(DeleteColumns const& v) { build("DECDC", "Delete columns", v.n); }
    void operator()(HorizontalPositionAbsolute const& v) { build("HPA", "Horizontal Position Absolute", v.n); }
    void operator()(HorizontalPositionRelative const& v) { build("HPR", "HOrizontal Position Relative", v.n); }
    void operator()(MoveCursorUp const& v) { build("CUU", "Move cursor up", v.n); }
    void operator()(MoveCursorDown const& v) { build("CUD", "Move cursor down", v.n); }
    void operator()(MoveCursorForward const& v) { build("CUF", "Move cursor forward", v.n); }
    void operator()(MoveCursorBackward const& v)  { build("CUB", "Move cursor backward", v.n); }
    void operator()(MoveCursorToColumn const& v) { build("CHA", "Move cursor to column", v.column); }
    void operator()(MoveCursorToBeginOfLine const& v) { build("\\r"); }
    void operator()(MoveCursorTo const& v) { build("CUP", "Move cursor to position", v.row, v.column); }
    void operator()(MoveCursorToLine const& v) { build("VPA", "Move cursor to line", v.row); }
    void operator()(MoveCursorToNextTab const& v) { build("\\t"); }
    void operator()(SaveCursor const& v) { build("DECSC", "Save cursor"); }
    void operator()(RestoreCursor const& v) { build("DECRC", "Restore cursor"); }
    void operator()(Index const& v) { build("IND", "Moves cursor down (possibly scrolling)"); }
    void operator()(ReverseIndex const& v) { build("RI", "Moves cursor up (possibly scrolling)"); }
    void operator()(BackIndex const& v) { build("DECBI", "Moves cursor left (possibly scrolling)"); }
    void operator()(ForwardIndex const& v) { build("DECFI", "Moves cursor right (possibly scrolling)"); }
    void operator()(SaveWindowTitle const& v) {
        build("WINMANIP", "Saves window title on stack.", 22, 0, 0);
    }
    void operator()(ResizeWindow const& v) {
        switch (v.unit)
        {
            case ResizeWindow::Unit::Pixels:
                build("WINMANIP", "Resize window", 4, v.height, v.width);
                break;
            case ResizeWindow::Unit::Characters:
                build("WINMANIP", "Resize window", 8, v.height, v.width);
                break;
        }
    }
    void operator()(RestoreWindowTitle const& v) {
        build("WINMANIP", "Restores window title from stack.", 23, 0, 0);
    }
    void operator()(SetForegroundColor const& v) { build("SGR", fmt::format("Select foreground color to {}", to_string(v.color))); }
    void operator()(SetBackgroundColor const& v) { build("SGR", fmt::format("Select background color to {}", to_string(v.color))); }
    void operator()(SetGraphicsRendition const& v) { build("SGR", fmt::format("Select style rendition to {}", to_string(v.rendition))); }
    void operator()(SetMode const& v) {
        if (v.enable)
            build("SM", fmt::format("Set mode {}", to_string(v.mode)), static_cast<unsigned>(v.mode));
        else
            build("RM", fmt::format("Reset mode {}", to_string(v.mode)), static_cast<unsigned>(v.mode));
    }
    void operator()(RequestMode const& v) {
        build("DECRQM", fmt::format("Reuqest mode {}", to_string(v.mode)), static_cast<unsigned>(v.mode));
    }
    void operator()(SetTopBottomMargin const& v) {
        build("DECSTBM", "Set top/bottom margin.", v.top, v.bottom);
    }
    void operator()(SetLeftRightMargin const& v) {
        build("DECSLRM", "Set left/right margin.", v.left, v.right);
    }
    void operator()(ScreenAlignmentPattern const& v) { build("DECALN", "Draw Screen Alignment Pattern."); }
    void operator()(SendMouseEvents const& v) {} // TODO
    void operator()(ApplicationKeypadMode const& v) {} // TODO
    void operator()(DesignateCharset const& v) {} // TODO
    void operator()(SingleShiftSelect const& v) {} // TODO
    void operator()(ChangeWindowTitle const& v) {} // TODO
    void operator()(SoftTerminalReset) { build("DECSTR", "Soft terminal reset."); }
    void operator()(AppendChar const& v) {
        pendingText_ += utf8::to_string(utf8::encode(v.ch));
    }

  private:
    bool withParameters_;
    bool withComment_;
    vector<string> result_;
    string pendingText_;

    void flushPendingText()
    {
        if (!pendingText_.empty())
        {
            result_.emplace_back(fmt::format("\"{}\"", escape(pendingText_)));
            pendingText_.clear();
        }
    }

    void build(string_view _mnemonic,
               string_view _comment = {},
               vector<unsigned> _args = {})
    {
        flushPendingText();
        string out;
        out += _mnemonic;
        if (withParameters_ & !_args.empty())
        {
            out += ' ';
            out += std::to_string(_args[0]);
            for (size_t i = 1; i < _args.size(); ++i)
            {
                out += ' ';
                out += std::to_string(_args[i]);
            }
        }
        if (withComment_ && !_comment.empty())
        {
            while (out.size() < 16)
                out += ' ';
            out += "; ";
            out += _comment;
        }
        result_.emplace_back(move(out));
    }

    void build(string_view _mnemonic, string_view _comment, unsigned _a1) { build(_mnemonic, _comment, vector{_a1}); }
    void build(string_view _mnemonic, string_view _comment, unsigned _a1, unsigned _a2) { build(_mnemonic, _comment, vector{_a1, _a2}); }
    void build(string_view _mnemonic, string_view _comment, unsigned _a1, unsigned _a2, unsigned _a3) { build(_mnemonic, _comment, vector{_a1, _a2, _a3}); }
};

vector<string> to_mnemonic(vector<Command> const& _commands, bool _withParameters, bool _withComment)
{
    return MnemonicBuilder{_withParameters, _withComment}.build(_commands);
}

string to_string(Command const& _command)
{
    auto const v = to_mnemonic(vector{_command}, true, false);
    if (!v.empty())
        return v.front();
    else
        return ""s;
}

}  // namespace terminal
