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
#include <terminal/Commands.h>
#include <terminal/Util.h>
#include <terminal/util/UTF8.h>

#include <fmt/format.h>

#include <algorithm>
#include <sstream>
#include <cassert>

using namespace std;
using fmt::format;

namespace terminal {

std::string setDynamicColorValue(RGBColor const& color)
{
    auto const r = static_cast<unsigned>(static_cast<float>(color.red) / 255.0f * 0xFFFF);
    auto const g = static_cast<unsigned>(static_cast<float>(color.green) / 255.0f * 0xFFFF);
    auto const b = static_cast<unsigned>(static_cast<float>(color.blue) / 255.0f * 0xFFFF);
    return fmt::format("rgb:{:04X}/{:04X}/{:04X}", r, g, b);
}

CursorShape makeCursorShape(string const& _name)
{
    string const name = [](string const& _input) {
        string output;
        transform(begin(_input), end(_input), back_inserter(output), [](auto ch) { return tolower(ch); });
        return output;
    }(_name);

    if (name == "block")
        return CursorShape::Block;
    else if (name == "rectangle")
        return CursorShape::Rectangle;
    else if (name == "underscore")
        return CursorShape::Underscore;
    else if (name == "bar")
        return CursorShape::Bar;
    else
        throw runtime_error{"Invalid cursor shape."};
}

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
        case Mode::FocusTracking:
            return "FocusTracking";
        case Mode::MouseExtended:
            return "MouseExtended";
        case Mode::MouseSGR:
            return "MouseSGR";
        case Mode::MouseURXVT:
            return "MouseURXVT";
        case Mode::MouseAlternateScroll:
            return "MouseAlternateScroll";
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

unsigned to_code(MouseProtocol protocol) noexcept
{
    return static_cast<unsigned>(protocol);
}

string to_string(MouseProtocol protocol)
{
    switch (protocol)
    {
        case MouseProtocol::X10:
            return "X10";
        case MouseProtocol::NormalTracking:
            return "NormalTracking";
        case MouseProtocol::ButtonTracking:
            return "ButtonTracking";
        case MouseProtocol::AnyEventTracking:
            return "AnyEventTracking";
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

    string build(Command const& _command)
    {
        visit(*this, _command);
        flushPendingText();
        assert(result_.size() == 1);
        return result_.back();
    }

    vector<string> result()
    {
        flushPendingText();
        return result_;
    }

    void operator()(Bell const&) { build("\\a"); }
    void operator()(FullReset const&) { build("RIS", "Reset to Initial state (hard reset)"); }
    void operator()(Linefeed const&) { build("\\n"); }
    void operator()(Backspace const&) { build("\\b"); }
    void operator()(DeviceStatusReport const&) { build("TODO:DeviceStatusReport"); }
    void operator()(ReportCursorPosition const&) { build("CPR", "Report cursor position"); }
    void operator()(ReportExtendedCursorPosition const&) { build("DECXCPR", "Report cursor position (extended)."); }
    void operator()(SendDeviceAttributes const&) { build("DA1", "Primary Device Attributes"); }
    void operator()(SendTerminalId const&) { build("SendTerminalId"); }
    void operator()(ClearToEndOfScreen const&) { build("ED", "Clear to end of screen", 0); }
    void operator()(ClearToBeginOfScreen const&) { build("ED", "Clear to begin of screen", 1); }
    void operator()(ClearScreen const&) { build("ED", "Clear screen", 2); }
    void operator()(ClearScrollbackBuffer const&) { build("ED", "Clear scrollback buffer", 3); }
    void operator()(EraseCharacters const& v) { build("ECH", "Erase characters", v.n); }
    void operator()(ScrollUp const& v) { build("SU", "Scroll up", v.n); }
    void operator()(ScrollDown const& v) { build("SD", "Scroll down", v.n); }
    void operator()(ClearToEndOfLine const&) { build("EL", "Clear to end of line", 0); }
    void operator()(ClearToBeginOfLine const&) { build("EL", "Clear to begin of line", 1); }
    void operator()(ClearLine const&)  { build("EL", "Clear line", 2); }
    void operator()(CursorNextLine const& v) { build("CNL", "Cursor Next Line", v.n); }
    void operator()(CursorPreviousLine const& v) { build("CNL", "Cursor Previous Line", v.n); }
    void operator()(InsertCharacters const& v) { build("ICH", "Insert Characters", v.n); }
    void operator()(InsertColumns const& v) { build("DECIC", "Insert Columns", v.n); }
    void operator()(InsertLines const& v) { build("IL", "Insert Lines", v.n); }
    void operator()(DeleteLines const& v) { build("DL", "Delete Lines", v.n); }
    void operator()(DeleteCharacters const& v) { build("DCH", "Delete characters", v.n); }
    void operator()(DeleteColumns const& v) { build("DECDC", "Delete columns", v.n); }
    void operator()(HorizontalPositionAbsolute const& v) { build("HPA", "Horizontal Position Absolute", v.n); }
    void operator()(HorizontalPositionRelative const& v) { build("HPR", "Horizontal Position Relative", v.n); }
    void operator()(HorizontalTabClear const& v) { build("TBC", "Horizontal Tab Clear", v.which); }
    void operator()(HorizontalTabSet) { build("HTS", "Horizontal Tab Set"); }
    void operator()(MoveCursorUp const& v) { build("CUU", "Move cursor up", v.n); }
    void operator()(MoveCursorDown const& v) { build("CUD", "Move cursor down", v.n); }
    void operator()(MoveCursorForward const& v) { build("CUF", "Move cursor forward", v.n); }
    void operator()(MoveCursorBackward const& v)  { build("CUB", "Move cursor backward", v.n); }
    void operator()(MoveCursorToColumn const& v) { build("CHA", "Move cursor to column", v.column); }
    void operator()(MoveCursorToBeginOfLine const&) { build("\\r"); }
    void operator()(MoveCursorTo const& v) { build("CUP", "Move cursor to position", v.row, v.column); }
    void operator()(MoveCursorToLine const& v) { build("VPA", "Move cursor to line", v.row); }
    void operator()(MoveCursorToNextTab const&) { build("\\t"); }
    void operator()(CursorBackwardTab const& v) { build("CBT", "Cursor Backward Tab", v.count); }
    void operator()(SaveCursor const&) { build("DECSC", "Save cursor"); }
    void operator()(RestoreCursor const&) { build("DECRC", "Restore cursor"); }
    void operator()(Index const&) { build("IND", "Moves cursor down (possibly scrolling)"); }
    void operator()(ReverseIndex const&) { build("RI", "Moves cursor up (possibly scrolling)"); }
    void operator()(BackIndex const&) { build("DECBI", "Moves cursor left (possibly scrolling)"); }
    void operator()(ForwardIndex const&) { build("DECFI", "Moves cursor right (possibly scrolling)"); }
    void operator()(SaveWindowTitle const&) {
        build("WINMANIP", "Saves window title on stack.", 22, 0, 0);
    }
    void operator()(ResizeWindow const& v) {
        switch (v.unit)
        {
            case ResizeWindow::Unit::Pixels:
                build("WINMANIP", "Resize window (in pixels)", 4, v.height, v.width);
                break;
            case ResizeWindow::Unit::Characters:
                build("WINMANIP", "Resize window (in chars)", 8, v.height, v.width);
                break;
        }
    }
    void operator()(RestoreWindowTitle const&) {
        build("WINMANIP", "Restores window title from stack.", 23, 0, 0);
    }
    void operator()(SetForegroundColor const& v) { build("SGR", fmt::format("Select foreground color to {}", to_string(v.color))); }
    void operator()(SetBackgroundColor const& v) { build("SGR", fmt::format("Select background color to {}", to_string(v.color))); }
    void operator()(SetGraphicsRendition const& v) { build("SGR", fmt::format("Select style rendition to {}", to_string(v.rendition))); }
    void operator()(SetMark const&) { build("SETMARK", "Sets vertical jump-mark in current line"); }
    void operator()(SetMode const& v) {
        if (v.enable)
            build("SM", fmt::format("Set mode {}", to_string(v.mode)), static_cast<unsigned>(v.mode));
        else
            build("RM", fmt::format("Reset mode {}", to_string(v.mode)), static_cast<unsigned>(v.mode));
    }
    void operator()(RequestMode const& v) {
        build("DECRQM", fmt::format("Reuqest mode {}", to_string(v.mode)), static_cast<unsigned>(v.mode));
    }
    void operator()(SetCursorStyle const& v) { build("DECSCUSR", fmt::format("Select cursor style to {} {}", to_string(v.display), to_string(v.shape))); }
    void operator()(SetTopBottomMargin const& v) {
		if (v.bottom.has_value())
	        build("DECSTBM", "Set top/bottom margin.", v.top.value_or(1), v.bottom.value());
		else
			build("DECSTBM", "Set top/bottom margin.", v.top.value_or(1));
    }
    void operator()(SetLeftRightMargin const& v) {
		if (v.right.has_value())
        	build("DECSLRM", "Set left/right margin.", v.left.value_or(1), v.right.value());
		else
			build("DECSLRM", "Set left/right margin.", v.left.value_or(1));
    }
    void operator()(ScreenAlignmentPattern) { build("DECALN", "Draw Screen Alignment Pattern."); }
    void operator()(SendMouseEvents const& v) {
        build(fmt::format("MOUSE({})", to_string(v.protocol)), "Send Mouse Events", v.enable);
    }
    void operator()(ApplicationKeypadMode const& v) {
        if (v.enable)
            build("DECKPAM", "Keypad Application Mode");
        else
            build("DECKPNM", "Keypad Numeric Mode");
    }
    void operator()(DesignateCharset) { build("TODO"); }
    void operator()(SingleShiftSelect const& v) {
        switch (v.table) {
            case CharsetTable::G0:
                build("SS0", "TODO: what's this?");
                break;
            case CharsetTable::G1:
                build("SS1", "TODO: what's this?");
                break;
            case CharsetTable::G2:
                build("SS2", "Maps G2 into GL for the next character.");
                break;
            case CharsetTable::G3:
                build("SS3", "Maps G3 into GL for the next character.");
                break;
        }
    }
    void operator()(ChangeWindowTitle const& v) {
        build("WINTITLE", fmt::format("Sets window title to {}", v.title));
    }
    void operator()(SoftTerminalReset) { build("DECSTR", "Soft terminal reset."); }
    void operator()(AppendChar const& v) {
        pendingText_ += utf8::to_string(utf8::encode(v.ch));
    }

    void operator()(SetDynamicColor const& v) {
        build("SETDYNCOLOR", fmt::format("{} {}", v.name, to_string(v.color)));
    }
    void operator()(ResetDynamicColor v) {
        build("RSTDYNCOLOR", fmt::format("{}", v.name));
    }
    void operator()(RequestDynamicColor v) {
        build("REQDYNCOLOR", fmt::format("{}", v.name));
    }
    void operator()(RequestTabStops) {
        build("DECTABSR");
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
            auto const a1 = '"' + move(pendingText_) + '"';
            pendingText_.clear();
            build("TEXT", "", vector{a1});
        }
    }

    template <typename T>
    void build(string_view _mnemonic,
               string_view _comment,
               vector<T> const& _args)
    {
        flushPendingText();
        string out;
        out += _mnemonic;
        if (withParameters_ & !_args.empty())
        {
            out += fmt::format(" {}", _args[0]);
            for (size_t i = 1; i < _args.size(); ++i)
                out += fmt::format(" {}", _args[i]);
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

    void build(string_view _mnemonic) { build(_mnemonic, "", vector<int>{}); }
    void build(string_view _mnemonic, string_view _comment) { build(_mnemonic, _comment, vector<int>{}); }
    template <typename T> void build(string_view _mnemonic, string_view _comment, T _a1) { build(_mnemonic, _comment, vector{_a1}); }
    void build(string_view _mnemonic, string_view _comment, unsigned _a1, unsigned _a2) { build(_mnemonic, _comment, vector{_a1, _a2}); }
    void build(string_view _mnemonic, string_view _comment, unsigned _a1, unsigned _a2, unsigned _a3) { build(_mnemonic, _comment, vector{_a1, _a2, _a3}); }
};

vector<string> to_mnemonic(vector<Command> const& _commands, bool _withParameters, bool _withComment)
{
    return MnemonicBuilder{_withParameters, _withComment}.build(_commands);
}

string to_mnemonic(Command const& _command, bool _withParameters, bool _withComment)
{
    return MnemonicBuilder{_withParameters, _withComment}.build(_command);
}

string to_string(Command const& _command)
{
    return MnemonicBuilder{true, false}.build(_command);
}

}  // namespace terminal
