// This file is part of the "libterminal" project, http://github.com/christianparpart/libterminal>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <fmt/format.h>
#include <terminal/Commands.h>
#include <terminal/UTF8.h>
#include <terminal/Util.h>

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
            [&](MoveCursorUp const& v) { return format("MoveCursorUp", v.n); },
            [&](MoveCursorDown const& v) { return format("MoveCursorDown", v.n); },
            [&](MoveCursorForward const& v) { return format("MoveCursorForward", v.n); },
            [&](MoveCursorBackward const& v) { return format("MoveCursorBackward", v.n); },
            [&](MoveCursorToColumn const& v) { return format("MoveCursorToColumn", v.column); },
            [&](MoveCursorToBeginOfLine) { return "MoveCursorToBeginOfLine"s; },
            [&](MoveCursorTo const& v) { return format("MoveCursorTo({}, {})", v.row, v.column); },
            [&](MoveCursorToNextTab) { return "MoveCursorToNextTab"s; },
            [&](HideCursor) { return "HideCursor"s; },
            [&](ShowCursor) { return "ShowCursor"s; },
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

}  // namespace terminal
