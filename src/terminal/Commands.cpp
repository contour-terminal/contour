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
#include <terminal/UTF8.h>
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
        case Mode::AutomaticLinefeed:
            return "AutomaticLinefeed";
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
        case Mode::CursorRestrictedToMargin:
            return "CursorRestrictedToMargin";
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

string to_string(Command const& _command)
{
    using namespace std::string_literals;

    return visit(
        overloaded{
            [&](Bell) { return "Bell"s; },
            [&](Linefeed) { return "Linefeed"s; },
            [&](Backspace) { return "Backspace"s; },
            [&](FullReset) { return "FullReset"s; },
            [&](DeviceStatusReport) { return "DeviceStatusReport"s; },
            [&](ReportCursorPosition) { return "ReportCursorPosition"s; },
            [&](ReportExtendedCursorPosition) { return "ReportExtendedCursorPosition"s; },
            [&](SendDeviceAttributes) { return "SendDeviceAttributes"s; },
            [&](SendTerminalId) { return "SendTerminalId"s; },
            [&](ClearToEndOfScreen) { return "ClearToEndOfScreen"s; },
            [&](ClearToBeginOfScreen) { return "ClearToBeginOfScreen"s; },
            [&](ClearScreen) { return "ClearScreen"s; },
            [&](ClearScrollbackBuffer) { return "ClearScrollbackBuffer"s; },
            [&](ScrollUp const& v) { return format("ScrollUp({})", v.n); },
            [&](ScrollDown const& v) { return format("ScrollDown({})", v.n); },
            [&](ClearToEndOfLine) { return "ClearToEndOfLine"s; },
            [&](ClearToBeginOfLine) { return "ClearToBeginOfLine"s; },
            [&](ClearLine) { return "ClearLine"s; },
            [&](CursorNextLine) { return "CursorNextLine"s; },
            [&](CursorPreviousLine) { return "CursorPreviousLine"s; },
            [&](InsertLines const& v) { return format("InsertLines({})", v.n); },
            [&](DeleteLines const& v) { return format("DeleteLines({})", v.n); },
            [&](DeleteCharacters const& v) { return format("DeleteCharacters({})", v.n); },
            [&](EraseCharacters const& v) { return format("EraseCharacters({})", v.n); },
            [&](MoveCursorUp const& v) { return format("MoveCursorUp({})", v.n); },
            [&](MoveCursorDown const& v) { return format("MoveCursorDown({})", v.n); },
            [&](MoveCursorForward const& v) { return format("MoveCursorForward({})", v.n); },
            [&](MoveCursorBackward const& v) { return format("MoveCursorBackward({})", v.n); },
            [&](MoveCursorToColumn const& v) { return format("MoveCursorToColumn({})", v.column); },
            [&](MoveCursorToBeginOfLine) { return "MoveCursorToBeginOfLine"s; },
            [&](MoveCursorTo const& v) { return format("MoveCursorTo({}, {})", v.row, v.column); },
            [&](MoveCursorToLine const& v) { return format("MoveCursorLine({})", v.row); },
            [&](MoveCursorToNextTab) { return "MoveCursorToNextTab"s; },
            [&](SaveCursor) { return "SaveCursor"s; },
            [&](RestoreCursor) { return "RestoreCursor"s; },
            [&](SetForegroundColor const& v) { return format("SetForegroundColor({})", to_string(v.color)); },
            [&](SetBackgroundColor const& v) { return format("SetBackgroundColor({})", to_string(v.color)); },
            [&](SetGraphicsRendition const& v) { return format("SetGraphicsRendition({})", to_string(v.rendition)); },
            [&](SetMode const& v) { return format("SetMode({}, {})", to_string(v.mode), v.enable); },
            [&](SendMouseEvents const& v) {
                return format("SendMouseEvents({}, {})", to_string(v.protocol), v.enable);
            },
            [&](AlternateKeypadMode const& v) { return format("AlternateKeypadMode({})", v.enable); },
            [&](Index) { return format("Index()"); },
            [&](ReverseIndex) { return format("ReverseIndex()"); },
            [&](BackIndex) { return format("BackIndex()"); },
            [&](ForwardIndex) { return format("ForwardIndex()"); },
            [&](DesignateCharset const& v) {
                return format("DesignateCharset({}, {})", to_string(v.table), to_string(v.charset));
            },
            [&](SingleShiftSelect const& v) { return format("SingleShiftSelect({})", to_string(v.table)); },
            [&](SetTopBottomMargin const& v) { return format("SetTopBottomMargin({}, {})", v.top, v.bottom); },
            [&](SetLeftRightMargin const& v) { return format("SetLeftRightMargin({}, {})", v.left, v.right); },
            [&](ScreenAlignmentPattern const& v) { return format("ScreenAlignmentPattern()"); },
            [&](AppendChar const& v) { return format("AppendChar({})", escape(utf8::to_string(utf8::encode(v.ch)))); },
            [&](ChangeWindowTitle const& v) { return format("ChangeWindowTitle(\"{}\")", v.title); },
            [&](ChangeIconName const& v) { return format("ChangeIconName(\"{}\")", v.name); },
        },
        _command);
}

struct MnemonicBuilder {
    bool withParameters;
    bool withComment;
    stringstream out;

    void build(string_view _mnemonic,
               string_view _comment = {},
               vector<unsigned> _args = {})
    {
        out <<_mnemonic;
        if (withParameters & !_args.empty())
        {
            out << ' ' << _args[0];
            for (size_t i = 1; i < _args.size(); ++i)
                out << ", " << _args[i];
        }
        if (withComment && !_comment.empty())
            out << "\t; " << _comment;
        out << "\n";
    }

    void build(string_view _mnemonic, string_view _comment, unsigned _a1) { build(_mnemonic, _comment, {_a1}); }

    void operator()(Bell const& v) { build("\\a"); }
    void operator()(FullReset const& v);
    void operator()(Linefeed const& v);
    void operator()(Backspace const& v);
    void operator()(DeviceStatusReport const& v);
    void operator()(ReportCursorPosition const& v);
    void operator()(ReportExtendedCursorPosition const& v);
    void operator()(SendDeviceAttributes const& v);
    void operator()(SendTerminalId const& v) {}
    void operator()(ClearToEndOfScreen const& v) {
        build("ED", "Clear to end of screen", 0);
    }
    void operator()(ClearToBeginOfScreen const& v) {
        build("ED", "Clear to begin of screen", 1);
    }
    void operator()(ClearScreen const& v) {
        build("ED", "Clear screen", 2);
    }
    void operator()(ClearScrollbackBuffer const& v) {
        build("ED", "Clear scrollback buffer", 3);
    }
    void operator()(EraseCharacters const& v);
    void operator()(ScrollUp const& v) {
        build("SU", "Scroll up", v.n);
    }
    void operator()(ScrollDown const& v) {
        build("SD", "Scroll down", v.n);
    }
    void operator()(ClearToEndOfLine const& v);
    void operator()(ClearToBeginOfLine const& v);
    void operator()(ClearLine const& v);
    void operator()(CursorNextLine const& v);
    void operator()(CursorPreviousLine const& v);
    void operator()(InsertLines const& v);
    void operator()(DeleteLines const& v);
    void operator()(DeleteCharacters const& v);
    void operator()(MoveCursorUp const& v);
    void operator()(MoveCursorDown const& v);
    void operator()(MoveCursorForward const& v);
    void operator()(MoveCursorBackward const& v);
    void operator()(MoveCursorToColumn const& v);
    void operator()(MoveCursorToBeginOfLine const& v);
    void operator()(MoveCursorTo const& v);
    void operator()(MoveCursorToLine const& v);
    void operator()(MoveCursorToNextTab const& v);
    void operator()(SaveCursor const& v);
    void operator()(RestoreCursor const& v);
    void operator()(Index const& v);
    void operator()(ReverseIndex const& v);
    void operator()(BackIndex const& v);
    void operator()(ForwardIndex const& v);
    void operator()(SetForegroundColor const& v);
    void operator()(SetBackgroundColor const& v);
    void operator()(SetGraphicsRendition const& v);
    void operator()(SetMode const& v);
    void operator()(SetTopBottomMargin const& v);
    void operator()(SetLeftRightMargin const& v);
    void operator()(ScreenAlignmentPattern const& v);
    void operator()(SendMouseEvents const& v);
    void operator()(AlternateKeypadMode const& v);
    void operator()(DesignateCharset const& v);
    void operator()(SingleShiftSelect const& v);
    void operator()(ChangeWindowTitle const& v);
    void operator()(ChangeIconName const& v);
    void operator()(AppendChar const& v);
};


}  // namespace terminal
