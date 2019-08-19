// This file is part of the "libterminal" project, http://github.com/christianparpart/libterminal>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <terminal/Color.h>
#include <terminal/Generator.h>
#include <terminal/UTF8.h>
#include <terminal/Util.h>

#include <fmt/format.h>

#include <array>
#include <fstream>
#include <numeric>
#include <string>

using namespace std;

namespace terminal {

Generator::~Generator()
{
    flush();
}

void Generator::operator()(std::vector<Command> const& commands)
{
    for (Command const& command : commands)
        (*this)(command);
}

constexpr optional<char> gnumber(CharsetTable table, Charset charset)
{
    array<char, 4> const std = {'(', ')', '*', '+'};

    switch (charset)
    {
        case Charset::Special:
        case Charset::UK:
        case Charset::USASCII:
        case Charset::German:
            return {std.at(static_cast<size_t>(table))};
    }
    return nullopt;
}

optional<char> finalChar(Charset charset)
{
    switch (charset)
    {
        case Charset::Special:
            return {'0'};
        case Charset::UK:
            return {'A'};
        case Charset::USASCII:
            return {'B'};
        case Charset::German:
            return {'K'};
    }
    return nullopt;
}

string Generator::flush(vector<int> _sgr)
{
    if (_sgr.empty())
        return "";

    auto const params =
        _sgr.size() != 1 || _sgr[0] != 0
            ? accumulate(begin(_sgr), end(_sgr), string{},
                         [](auto a, auto b) {
                             return a.empty() ? fmt::format("{}", b) : fmt::format("{};{}", a, b);
                         })
            : string();

    return fmt::format("\033[{}m", params);
}

void Generator::flush()
{
    if (!sgr_.empty())
    {
        auto const f = flush(move(sgr_));
        sgr_.clear();
        writer_(f.data(), f.size());
    }
}

void Generator::sgr_add(int n)
{
    if (n == 0)
    {
        sgr_.clear();
        sgr_.push_back(n);
    }
    else
    {
        if (sgr_.empty() || sgr_.back() != n)
            sgr_.push_back(n);

        if (sgr_.size() == 16)
        {
            write(flush(move(sgr_)));
            sgr_.clear();
        }
    }
}

void Generator::operator()(Command const& command)
{
    auto const pairOrNone = [](int _default, int _a, int _b) -> string {
        if (_a == _default && _b == _default)
            return "";
        else if (_a == _default)
            return fmt::format(";{}", _b);
        else if (_b == _default)
            return fmt::format("{};", _a);
        else
            return fmt::format("{};{}", _a, _b);
    };

    visit(overloaded{
        [&](Bell) { write("\x07"); },
        [&](Linefeed) { write("\n"); },
        [&](Backspace) { write("\x08"); },
        [&](FullReset) { write("\033c"); },
        [&](DeviceStatusReport) { write("\033[5n"); },
        [&](ReportCursorPosition) { write("\033[6n"); },
        [&](SendDeviceAttributes) { write("\033[c"); },
        [&](SendTerminalId) { write("\033[>c"); },
        [&](ClearToEndOfScreen) { write("\033[0J"); },
        [&](ClearToBeginOfScreen) { write("\033[1J"); },
        [&](ClearScreen) { write("\033[2J"); },
        [&](ClearScrollbackBuffer) { write("\033[3J"); },
        [&](ScrollUp const& up) { write("\033[{}S", up.n); },
        [&](ScrollDown const& down) { write("\033[{}T", down.n); },
        [&](ClearToEndOfLine) { write("\033[K"); },
        [&](ClearToBeginOfLine) { write("\033[1K"); },
        [&](ClearLine) { write("\033[2K"); },
        [&](InsertLines const& lines) { write("\033[{}L", lines.n); },
        [&](DeleteLines const& lines) { write("\033[{}M", lines.n); },
        [&](DeleteCharacters const& chars) { write("\033[{}P", chars.n); },
        [&](MoveCursorUp const& up) { write("\033[{}A", up.n); },
        [&](MoveCursorDown const& down) { write("\033[{}B", down.n); },
        [&](MoveCursorForward const& fwd) { write("\033[{}C", fwd.n); },
        [&](MoveCursorBackward const& back) { write("\033[{}D", back.n); },
        [&](MoveCursorToColumn const& to) { write("\033[{}G", to.column); },
        [&](MoveCursorToBeginOfLine) { write("\r"); },
        [&](MoveCursorTo const& to) { write("\033[{}H", pairOrNone(1, to.row, to.column)); },
        [&](MoveCursorToNextTab) { write("\t"); },
        [&](HideCursor) { write("\033[?25l"); },
        [&](ShowCursor) { write("\033[?25h"); },
        [&](SaveCursor) { write("\0337"); },
        [&](RestoreCursor) { write("\0338"); },
        [&](SetForegroundColor const& v) {
            if (v.color != currentForegroundColor_)
            {
                currentForegroundColor_ = v.color;
                if (holds_alternative<IndexedColor>(v.color))
                {
                    auto const colorValue = get<IndexedColor>(v.color);
                    if (static_cast<unsigned>(colorValue) < 8)
                        sgr_add(30 + static_cast<unsigned>(colorValue));
                    else
                    {
                        sgr_add(38);
                        sgr_add(5);
                        sgr_add(static_cast<unsigned>(colorValue));
                    }
                }
                else if (holds_alternative<DefaultColor>(v.color))
                    sgr_add(39);
                else if (holds_alternative<BrightColor>(v.color))
                    sgr_add(90 + static_cast<unsigned>(get<BrightColor>(v.color)));
            }
        },
        [&](SetBackgroundColor const& v) {
            if (v.color != currentBackgroundColor_)
            {
                currentBackgroundColor_ = v.color;
                if (holds_alternative<IndexedColor>(v.color))
                {
                    auto const colorValue = get<IndexedColor>(v.color);
                    if (static_cast<unsigned>(colorValue) < 8)
                        sgr_add(40 + static_cast<unsigned>(colorValue));
                    else
                    {
                        sgr_add(48);
                        sgr_add(5);
                        sgr_add(static_cast<unsigned>(colorValue));
                    }
                }
                else if (holds_alternative<DefaultColor>(v.color))
                    sgr_add(49);
                else if (holds_alternative<BrightColor>(v.color))
                    sgr_add(100 + static_cast<unsigned>(get<BrightColor>(v.color)));
            }
        },
        [&](SetMode mode) { write("\033[{}{}", to_code(mode.mode), mode.enable ? 'h' : 'l'); },
        [&](SetTopBottomMargin margin) { write("\033[{};{}r", margin.top, margin.bottom); },
        [&](SetLeftRightMargin margin) { write("\033[{};{}s", margin.left, margin.right); },
        [&](ScreenAlignmentPattern) { write("\033#8"); },
        [&](SendMouseEvents v) { write("\033[?{}{}", to_code(v.protocol), v.enable ? 'h' : 'l'); },
        [&](AlternateKeypadMode v) { write("\033{}", v.enable ? '=' : '>'); },
        [&](Index) { write("\033D"); },
        [&](ReverseIndex) { write("\033M"); },
        [&](ForwardIndex) { write("\0339"); },
        [&](BackIndex) { write("\0336"); },
        [&](SetGraphicsRendition const& v) {
            // TODO: add context-aware caching to avoid double-setting
            sgr_add(static_cast<int>(v.rendition));
            if (v.rendition == GraphicsRendition::Reset)
            {
                currentForegroundColor_ = DefaultColor{};
                currentBackgroundColor_ = DefaultColor{};
            }
        },
        [&](DesignateCharset v) {
            if (auto g = gnumber(v.table, v.charset); g.has_value())
                if (auto f = finalChar(v.charset); f.has_value())
                    write("\033{}{}", *g, *f);
        },
        [&](SingleShiftSelect v) {
            switch (v.table)
            {
                case CharsetTable::G2:
                    write("\033N");
                    break;
                case CharsetTable::G3:
                    write("\033O");
                    break;
                default:
                    break;
            }
        },
        [&](AppendChar const& v) { write(v.ch); },
        [&](ChangeIconName const& v) { write("\033]1;{}\x9c", v.name); },
        [&](ChangeWindowTitle const& v) { write("\033]2;{}\x9c", v.title); },
    }, command);
}

}  // namespace terminal
