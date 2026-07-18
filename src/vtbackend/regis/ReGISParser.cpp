// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISParser.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>

using crispy::point;

namespace vtbackend::regis
{

namespace
{
    [[nodiscard]] bool isPvDigit(char ch) noexcept
    {
        return ch >= '0' && ch <= '7';
    }
    [[nodiscard]] bool isDigit(char ch) noexcept
    {
        return ch >= '0' && ch <= '9';
    }
    [[nodiscard]] char upper(char ch) noexcept
    {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }

    /// Converts a wire-supplied text-cell extent to a clamped pixel count. Clamps the @c double to
    /// [1, @ref MaxTextCellExtent] BEFORE the cast, so a negative or huge value can never be an
    /// out-of-range (undefined) float-to-unsigned conversion, and a zero/sub-unit multiplier can never
    /// collapse the cell to 0 (which would make all later text vanish).
    [[nodiscard]] unsigned clampCellExtent(double value) noexcept
    {
        return static_cast<unsigned>(std::clamp(value, 1.0, static_cast<double>(MaxTextCellExtent)));
    }

    /// Clamps a graphics-cursor coordinate to [-@ref MaxCanvasCoord, @ref MaxCanvasCoord], keeping the
    /// persistent cursor far from signed-integer overflow without affecting any on-canvas position.
    [[nodiscard]] int clampCanvasCoord(int v) noexcept
    {
        return std::clamp(v, -MaxCanvasCoord, MaxCanvasCoord);
    }

    [[nodiscard]] crispy::point clampCanvasPoint(crispy::point p) noexcept
    {
        return { .x = clampCanvasCoord(p.x), .y = clampCanvasCoord(p.y) };
    }

    // Fork-bomb guards for macrograph expansion: untrusted input can define self-referential macros.
    constexpr std::size_t MaxMacroDepth = 32;
    constexpr std::size_t MaxMacroOutput = 256 * 1024;

    /// Expands ReGIS macrographs in @p input, appending the result to @p out.
    ///
    /// @c \@:X...\@; defines macro @c X (stored in @p macros and removed from the stream), @c \@X
    /// invokes it (late-bound, recursively expanded), and @c \@. clears all. Definitions persist in
    /// @p macros across DCS strings. Expansion stops at the depth and output caps.
    void expandMacrographs(std::string& out,
                           std::string_view input,
                           std::unordered_map<char, std::string>& macros,
                           std::size_t depth)
    {
        if (depth > MaxMacroDepth)
            return;
        auto i = std::size_t { 0 };
        while (i < input.size() && out.size() < MaxMacroOutput)
        {
            auto const c = input[i];
            if (c == '@' && i + 1 < input.size())
            {
                auto const next = input[i + 1];
                if (next == ':' && i + 2 < input.size())
                {
                    auto const name = input[i + 2];
                    auto const bodyStart = i + 3;
                    auto const end = input.find("@;", bodyStart);
                    if (end == std::string_view::npos)
                        break;
                    macros[name] = std::string(input.substr(bodyStart, end - bodyStart));
                    i = end + 2;
                    continue;
                }
                if (next == '.')
                {
                    macros.clear();
                    i += 2;
                    continue;
                }
                if (std::isalpha(static_cast<unsigned char>(next)))
                {
                    if (auto const it = macros.find(next); it != macros.end())
                    {
                        auto const body = it->second; // copy: a nested define may rehash the map
                        expandMacrographs(out, body, macros, depth + 1);
                    }
                    i += 2;
                    continue;
                }
            }
            out.push_back(c);
            ++i;
        }
    }
} // namespace

ReGISParser::ReGISParser(ReGISContext& context,
                         ReGISRasterizer& canvas,
                         std::shared_ptr<ReGISTextRasterizer const> textRasterizer,
                         ReGISEvents& events,
                         std::function<void()> onCommit):
    _context { context },
    _canvas { canvas },
    _textRasterizer { std::move(textRasterizer) },
    _events { events },
    _onCommit { std::move(onCommit) }
{
}

void ReGISParser::pass(char ch)
{
    _buffer.push_back(ch);
}

void ReGISParser::pass(std::string_view bytes)
{
    _buffer.append(bytes);
}

void ReGISParser::finalize()
{
    // Expand macrographs first: definitions persist in the context, invocations are substituted, so
    // the grammar parser never sees an @.
    auto expanded = std::string {};
    expandMacrographs(expanded, _buffer, _context.macrographs, 0);
    _buffer = std::move(expanded);

    _pos = 0;
    _drew = false;
    parseAll();
    // Commit when the payload drew something or when the canvas was cleared by a reset -- the latter
    // must still refresh the grid so the previously committed graphics disappear.
    if ((_drew || _committedStateDirty) && _onCommit)
        _onCommit();
}

bool ReGISParser::parse(std::string_view regis,
                        ReGISContext& context,
                        ReGISRasterizer& canvas,
                        ReGISTextRasterizer const& textRasterizer,
                        ReGISEvents& events)
{
    // Non-owning shared_ptr: this test entry point owns the rasterizer's lifetime itself, so the
    // parser must not delete it. (Production hands the parser a real owning shared_ptr.)
    auto const nonOwning =
        std::shared_ptr<ReGISTextRasterizer const> { std::shared_ptr<void> {}, &textRasterizer };
    auto parser = ReGISParser { context, canvas, nonOwning, events, [] {} };
    parser.pass(regis);
    parser.finalize();
    return parser._drew;
}

// {{{ scanning

void ReGISParser::skipWhitespace() noexcept
{
    while (!atEnd())
    {
        auto const c = peek();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            advance();
        else
            break;
    }
}

std::optional<double> ReGISParser::scanNumber() noexcept
{
    auto sign = 1.0;
    auto hadSign = false;
    if (peek() == '+')
    {
        advance();
        hadSign = true;
    }
    else if (peek() == '-')
    {
        advance();
        sign = -1.0;
        hadSign = true;
    }
    if (!isDigit(peek()) && peek() != '.')
        return hadSign ? std::optional<double> { 0.0 } : std::nullopt;
    auto value = 0.0;
    while (isDigit(peek()))
        value = (value * 10.0) + (advance() - '0');
    if (peek() == '.')
    {
        advance();
        auto scale = 0.1;
        while (isDigit(peek()))
        {
            value += (advance() - '0') * scale;
            scale *= 0.1;
        }
    }
    return sign * value;
}

unsigned ReGISParser::scanUnsigned() noexcept
{
    skipWhitespace();
    auto value = 0u;
    while (isDigit(peek()))
        value = (value * 10u) + static_cast<unsigned>(advance() - '0');
    return value;
}

std::string ReGISParser::scanQuotedString() noexcept
{
    auto const quote = advance(); // opening ' or "
    auto result = std::string {};
    while (!atEnd())
    {
        auto const c = advance();
        if (c == quote)
        {
            // A doubled quote is an escaped quote inside the string.
            if (peek() == quote)
                result.push_back(advance());
            else
                break;
        }
        else
            result.push_back(c);
    }
    return result;
}

// }}}

// {{{ coordinates

ReGISParser::Coord ReGISParser::scanCoordinate() noexcept
{
    // The opening '[' has already been consumed; consume through the closing ']'.
    auto coord = Coord {};
    skipWhitespace();
    if (peek() != ',' && peek() != ']')
    {
        auto const before = _pos;
        coord.xRelative = peek() == '+' || peek() == '-';
        coord.x = scanNumber();
        if (coord.x && _pos == before)
            coord.x = std::nullopt;
    }
    skipWhitespace();
    if (peek() == ',')
    {
        advance();
        skipWhitespace();
        if (peek() != ']')
        {
            coord.yRelative = peek() == '+' || peek() == '-';
            coord.y = scanNumber();
        }
    }
    skipWhitespace();
    if (peek() == ']')
        advance();
    return coord;
}

point ReGISParser::resolveCoordinate(Coord const& coord) const noexcept
{
    auto p = _context.position;
    if (coord.x)
        p.x = coord.xRelative ? p.x + _context.userDeltaToPixel(*coord.x, 0).x
                              : _context.userToPixel(*coord.x, 0).x;
    if (coord.y)
        p.y = coord.yRelative ? p.y + _context.userDeltaToPixel(0, *coord.y).y
                              : _context.userToPixel(0, *coord.y).y;
    return clampCanvasPoint(p);
}

void ReGISParser::moveTo(point p) noexcept
{
    _context.position = clampCanvasPoint(p);
}

void ReGISParser::drawLineTo(point p) noexcept
{
    p = clampCanvasPoint(p);
    _canvas.plotLine(_context.currentPen(), _context.position, p);
    _context.position = p;
    _drew = true;
}

// }}}

// {{{ colour

std::optional<RGBColor> ReGISParser::parseColorSpecParens() noexcept
{
    advance(); // '('
    auto named = std::optional<RGBColor> {};
    auto h = 0u;
    auto l = 0u;
    auto s = 0u;
    auto r = 0u;
    auto g = 0u;
    auto b = 0u;
    auto hasHue = false;
    auto hasSat = false;
    auto hasLight = false;
    auto hasRgb = false;
    while (!atEnd())
    {
        skipWhitespace();
        auto const c = peek();
        if (c == ')')
        {
            advance();
            break;
        }
        if (std::isalpha(static_cast<unsigned char>(c)))
        {
            auto const letter = upper(advance());
            skipWhitespace();
            if (isDigit(peek()))
            {
                auto const value = scanUnsigned();
                switch (letter)
                {
                    case 'H':
                        h = value;
                        hasHue = true;
                        break;
                    case 'S':
                        s = value;
                        hasSat = true;
                        break;
                    case 'L':
                        l = value;
                        hasLight = true;
                        break;
                    case 'R':
                        r = value;
                        hasRgb = true;
                        break;
                    case 'G':
                        g = value;
                        hasRgb = true;
                        break;
                    case 'B':
                        b = value;
                        hasRgb = true;
                        break;
                    default: break;
                }
            }
            else
            {
                // A letter not followed by a number is a named colour (D/R/G/B/C/Y/M/W).
                for (auto const& nc: NamedColors)
                    if (nc.letter == letter)
                        named = nc.rgb;
            }
        }
        else
            advance();
    }
    if (named)
        return named;
    if (hasRgb)
        return rgbPercentToRgb(r, g, b);
    if (hasHue || hasSat)
        return hlsToRgb(h, l, s);
    if (hasLight)
        return grayToRgb(l);
    return std::nullopt;
}

unsigned ReGISParser::parseColorReference() noexcept
{
    skipWhitespace();
    if (peek() == '(')
    {
        if (auto const color = parseColorSpecParens())
            return _context.registers.findClosest(*color);
        return _context.foregroundRegister;
    }
    return scanUnsigned();
}

// }}}

// {{{ options

void ReGISParser::applyWriteOption(char option)
{
    switch (upper(option))
    {
        case 'I': _context.foregroundRegister = parseColorReference(); break;
        case 'M':
            _context.pixelVectorMultiplier = std::clamp(scanUnsigned(), 1u, MaxPixelVectorMultiplier);
            break;
        case 'L': _context.lineWidth = std::clamp(static_cast<int>(scanUnsigned()), 1, MaxLineWidth); break;
        case 'V': _context.writingMode = WritingMode::Overlay; break;
        case 'R': _context.writingMode = WritingMode::Replace; break;
        case 'C': _context.writingMode = WritingMode::Complement; break;
        case 'E': _context.writingMode = WritingMode::Erase; break;
        case 'N': _context.negativePattern = scanUnsigned() != 0; break;
        case 'F': static_cast<void>(scanUnsigned()); break; // plane mask (ignored)
        case 'A': static_cast<void>(scanUnsigned()); break; // blink (ignored)
        case 'S': {
            // Shading: W(S0)/W(S1) toggle, W(S[y]) a horizontal reference, W(S(X)[x]) a vertical one,
            // W(S'c') a shading character (rendered as solid here).
            skipWhitespace();
            if (peek() == '(')
            {
                advance();
                skipWhitespace();
                if (upper(peek()) == 'X')
                {
                    advance();
                    _context.shadingVertical = true;
                }
                skipWhitespace();
                if (peek() == ')')
                    advance();
                _context.shadingEnabled = true;
                skipWhitespace();
                if (peek() == '[')
                {
                    advance();
                    auto const ref = resolveCoordinate(scanCoordinate());
                    _context.shadingReference = _context.shadingVertical ? ref.x : ref.y;
                }
            }
            else if (peek() == '[')
            {
                advance();
                auto const ref = resolveCoordinate(scanCoordinate());
                _context.shadingVertical = false;
                _context.shadingReference = ref.y;
                _context.shadingEnabled = true;
            }
            else if (peek() == '\'' || peek() == '"')
            {
                static_cast<void>(scanQuotedString()); // shading glyph -> solid shading
                _context.shadingEnabled = true;
            }
            else
            {
                // W(S1)/W(S0). Enabling with no explicit reference defaults to the horizontal line at
                // the cursor's current Y, per the VT340 manual.
                auto const enabled = scanUnsigned() != 0;
                _context.shadingEnabled = enabled;
                if (enabled)
                {
                    _context.shadingVertical = false;
                    _context.shadingReference = _context.position.y;
                }
            }
            break;
        }
        case 'P': {
            skipWhitespace();
            if (peek() == '(')
            {
                advance();
                skipWhitespace();
                if (upper(peek()) == 'M')
                {
                    advance();
                    _context.patternMultiplier = std::max(1u, scanUnsigned());
                }
                skipWhitespace();
                if (peek() == ')')
                    advance();
            }
            else if (isDigit(peek()))
            {
                auto const n = static_cast<unsigned>(advance() - '0');
                if (n < StandardPattern.size())
                    _context.pattern = StandardPattern[n];
            }
            break;
        }
        default: break;
    }
}

void ReGISParser::applyScreenOption(char option)
{
    switch (upper(option))
    {
        case 'E':
            _canvas.eraseTo(_context.backgroundColor());
            _drew = true;
            break;
        case 'A': {
            // Two absolute user-coordinate corners define the addressing window.
            skipWhitespace();
            if (peek() == '[')
            {
                advance();
                auto const c1 = scanCoordinate();
                if (c1.x)
                    _context.window.x0 = *c1.x;
                if (c1.y)
                    _context.window.y0 = *c1.y;
            }
            skipWhitespace();
            if (peek() == '[')
            {
                advance();
                auto const c2 = scanCoordinate();
                if (c2.x)
                    _context.window.x1 = *c2.x;
                if (c2.y)
                    _context.window.y1 = *c2.y;
            }
            break;
        }
        case 'I':
            _context.backgroundRegister = parseColorReference();
            _context.backgroundOpaque = true;
            break;
        case 'M':
            while (!atEnd())
            {
                skipWhitespace();
                auto c = peek();
                if (upper(c) == 'A') // "address registers only" prefix
                {
                    advance();
                    skipWhitespace();
                    c = peek();
                }
                if (!isDigit(c))
                    break;
                auto const index = scanUnsigned();
                skipWhitespace();
                if (peek() == '(')
                {
                    if (auto const color = parseColorSpecParens())
                        _context.registers.set(index, *color);
                }
            }
            break;
        case 'P': static_cast<void>(scanUnsigned()); break; // page select (single page for now)
        case 'C': static_cast<void>(scanUnsigned()); break; // cursor visibility (ignored)
        case 'N': static_cast<void>(scanUnsigned()); break; // reverse video (ignored)
        default: break;
    }
}

void ReGISParser::applyPositionOption(char option, Command command)
{
    switch (upper(option))
    {
        case 'B':
        case 'S': _context.positionStack.push_back(_context.position); break;
        case 'E':
            if (!_context.positionStack.empty())
            {
                // Position stacks are LIFO and nestable, so (E) closes back to the most recently
                // saved point and pops only that frame; (V) draws the closing edge. (Using front() +
                // clear() would restore to the outermost point and discard every nested frame at once.)
                auto const saved = _context.positionStack.back();
                _context.positionStack.pop_back();
                if (command == Command::Vector)
                    drawLineTo(saved);
                else
                    moveTo(saved);
            }
            break;
        case 'W':
            skipWhitespace();
            if (peek() == '(')
                parseOptionSet(Command::Write);
            break;
        case 'P': static_cast<void>(scanUnsigned()); break; // page select
        default: break;
    }
}

void ReGISParser::applyCurveOption(char option)
{
    switch (upper(option))
    {
        case 'C': _curveCenterMode = true; break;
        case 'A':
            skipWhitespace();
            _curveArc = scanNumber();
            break;
        case 'B':
        case 'S':
            _collectingCurve = true;
            _curveClosed = upper(option) == 'B';
            _curvePoints.assign(1, _context.position);
            break;
        case 'E':
            if (_collectingCurve)
            {
                _collectingCurve = false;
                if (_curvePoints.size() >= 2)
                {
                    _canvas.plotCurve(_context.currentPen(), _curvePoints, _curveClosed);
                    // A closed curve returns the cursor to its start; an open one ends at the last point.
                    _context.position = _curveClosed ? _curvePoints.front() : _curvePoints.back();
                    _drew = true;
                }
            }
            break;
        case 'W':
            skipWhitespace();
            if (peek() == '(')
                parseOptionSet(Command::Write);
            break;
        default: break;
    }
}

void ReGISParser::drawCurveItem(crispy::point p)
{
    // A bracketed coordinate under C draws a circle or arc. With (C) the bracket is the centre and
    // the current position lies on the circumference; otherwise the current position is the centre.
    auto const pen = _context.currentPen();
    auto const center = _curveCenterMode ? p : _context.position;
    auto const anchor = _curveCenterMode ? _context.position : p;
    auto const radius = static_cast<int>(std::lround(std::hypot(center.x - anchor.x, center.y - anchor.y)));
    if (_curveArc)
    {
        auto const startDegrees =
            std::atan2(static_cast<double>(center.y - anchor.y), static_cast<double>(anchor.x - center.x))
            * 180.0 / std::numbers::pi;
        _canvas.plotArc(pen, center, radius, startDegrees, *_curveArc);
    }
    else
        _canvas.plotCircle(pen, center, radius);
    _drew = true;
}

void ReGISParser::applyTextOption(char option)
{
    switch (upper(option))
    {
        case 'S': {
            if (!applyBracketedCellSize())
            {
                auto const n = scanUnsigned();
                if (n < StandardTextSize.size())
                {
                    _context.textCellWidth = StandardTextSize[n].width;
                    _context.textCellHeight = StandardTextSize[n].height;
                }
            }
            break;
        }
        case 'U': // unit cell; approximated by the display cell in this phase
            applyBracketedCellSize();
            break;
        case 'H': {
            // Height multiplier: the cell height becomes factor x the base character height, matching
            // xterm (graphics_regis.c). Assigning -- rather than multiplying the current height in
            // place -- keeps the size fixed across repeated T(H) uses instead of compounding, and the
            // multiplier is clamped before the multiply so it cannot overflow.
            auto const factor = std::min(scanUnsigned(), MaxTextHeightMultiplier);
            if (factor > 0)
                _context.textCellHeight =
                    std::min(factor * static_cast<unsigned>(StandardTextSize[0].height), MaxTextCellExtent);
            break;
        }
        case 'M': {
            skipWhitespace();
            if (peek() == '[')
            {
                advance();
                auto const mult = scanCoordinate();
                if (mult.x)
                    _context.textCellWidth =
                        clampCellExtent(static_cast<double>(_context.textCellWidth) * *mult.x);
                if (mult.y)
                    _context.textCellHeight =
                        clampCellExtent(static_cast<double>(_context.textCellHeight) * *mult.y);
            }
            break;
        }
        case 'D':
            skipWhitespace();
            if (auto const angle = scanNumber())
                _context.textDirection = static_cast<int>(std::lround(*angle));
            break;
        case 'I':
            skipWhitespace();
            if (auto const angle = scanNumber())
                _context.textSlant = static_cast<int>(std::lround(*angle));
            break;
        case 'A': static_cast<void>(scanUnsigned()); break; // alphabet select (L-loaded sets: later)
        case 'B':
        case 'E': break; // temporary text controls (applied persistently for now)
        default: break;
    }
}

bool ReGISParser::applyBracketedCellSize()
{
    skipWhitespace();
    if (peek() != '[')
        return false;
    advance(); // consume '['
    auto const cell = scanCoordinate();
    if (cell.x && *cell.x >= 1.0)
        _context.textCellWidth = clampCellExtent(*cell.x);
    if (cell.y && *cell.y >= 1.0)
        _context.textCellHeight = clampCellExtent(*cell.y);
    return true;
}

void ReGISParser::drawText(std::string_view text)
{
    auto pen = _context.currentPen();
    pen.shade = false; // text glyphs are never shaded to a reference line
    // Text cells are specified in logical pixels; scale them onto the supersampled canvas so the
    // glyph is rasterized at full canvas resolution (crisp) rather than the small logical cell size.
    auto const cellWidth = static_cast<int>(_context.textCellWidth * _context.supersample);
    auto const cellHeight = static_cast<int>(_context.textCellHeight * _context.supersample);
    auto const radians = static_cast<double>(_context.textDirection) * std::numbers::pi / 180.0;
    auto const advanceX = static_cast<int>(std::lround(std::cos(radians) * cellWidth));
    auto const advanceY = -static_cast<int>(std::lround(std::sin(radians) * cellHeight)); // y grows down
    auto const cellSize = ImageSize { Width(cellWidth), Height(cellHeight) };
    for (auto const ch: text)
    {
        if (auto const glyph = _textRasterizer->rasterize(static_cast<char32_t>(ch), cellSize))
            _canvas.blendCoverage(pen, _context.position, glyph->size, glyph->coverage);
        _context.position = clampCanvasPoint(
            point { .x = _context.position.x + advanceX, .y = _context.position.y + advanceY });
    }
    _drew = true;
}

void ReGISParser::parseOptionSet(Command command, int depth)
{
    advance(); // '('
    while (!atEnd())
    {
        skipWhitespace();
        auto const c = peek();
        if (c == ')')
        {
            advance();
            break;
        }
        if (c == '(')
        {
            // Guard the parser-thread stack: a pathological run of nested '(' would otherwise recurse
            // one frame per byte until it overflows. Beyond the cap, consume the '(' without recursing.
            if (depth < MaxOptionSetDepth)
                parseOptionSet(command, depth + 1); // nested option set
            else
                advance();
            continue;
        }
        auto const option = advance();
        switch (command)
        {
            case Command::Write: applyWriteOption(option); break;
            case Command::ScreenCmd: applyScreenOption(option); break;
            case Command::Position:
            case Command::Vector: applyPositionOption(option, command); break;
            case Command::Curve: applyCurveOption(option); break;
            case Command::Report: applyReportOption(option); break;
            case Command::Text: applyTextOption(option); break;
            default: break;
        }
    }
}

void ReGISParser::applyReportOption(char option)
{
    switch (upper(option))
    {
        case 'P': {
            skipWhitespace();
            if (peek() == '(')
            {
                // R(P(I...)) requests the interactive locator position; Contour reports it at once
                // rather than blocking. Any inner argument (e.g. movement multipliers) is skipped.
                advance();
                auto const interactive = upper(peek()) == 'I';
                auto depth = 1;
                while (!atEnd() && depth > 0)
                {
                    auto const c = advance();
                    depth += (c == '(') - (c == ')');
                }
                if (interactive)
                {
                    if (auto const locator = _events.locatorPosition())
                        _events.reply(std::format("0[{},{}]\r", locator->first, locator->second));
                    else
                    {
                        auto const [userX, userY] = _context.pixelToUser(_context.position);
                        _events.reply(std::format("0[{},{}]\r",
                                                  static_cast<int>(std::lround(userX)),
                                                  static_cast<int>(std::lround(userY))));
                    }
                }
                break;
            }
            auto const [userX, userY] = _context.pixelToUser(_context.position);
            _events.reply(std::format(
                "[{},{}]\r", static_cast<int>(std::lround(userX)), static_cast<int>(std::lround(userY))));
            break;
        }
        case 'E': _events.reply("0\r"); break; // last error: none
        case 'I':
            skipWhitespace();
            static_cast<void>(scanUnsigned()); // input mode select (one-shot/multiple)
            _events.reply("\r");
            break;
        case 'L': _events.reply("A0\r"); break; // current alphabet: the built-in set
        case 'M':
            // Macrograph storage report is answered in the macrograph phase; acknowledge for now.
            skipWhitespace();
            if (peek() == '(')
            {
                auto depth = 0;
                do
                {
                    auto const c = advance();
                    depth += (c == '(') - (c == ')');
                } while (!atEnd() && depth > 0);
            }
            _events.reply("\r");
            break;
        default: break;
    }
}

// }}}

// {{{ items

void ReGISParser::parseCoordinateItem(Command command)
{
    advance(); // '['
    auto const coord = scanCoordinate();
    auto const p = resolveCoordinate(coord);
    if (_fillSink)
    {
        _fillSink->push_back(p);
        _context.position = p;
        return;
    }
    if (_collectingCurve)
    {
        _curvePoints.push_back(p);
        _context.position = p;
        return;
    }
    switch (command)
    {
        case Command::Vector:
            if (coord.empty())
            {
                _canvas.plotDot(_context.currentPen(), _context.position);
                _drew = true;
            }
            else
                drawLineTo(p);
            break;
        case Command::Position: moveTo(p); break;
        case Command::Curve: drawCurveItem(p); break;
        default: moveTo(p); break;
    }
}

void ReGISParser::parsePixelVectorRun(Command command)
{
    while (!atEnd() && isPvDigit(peek()))
    {
        auto const digit = static_cast<size_t>(advance() - '0');
        auto const delta = PixelVectorDelta[digit];
        // The pixel-vector step is one logical pixel per unit; scale onto the supersampled canvas.
        auto const multiplier = static_cast<int>(_context.pixelVectorMultiplier * _context.supersample);
        auto next = clampCanvasPoint(point { .x = _context.position.x + (delta.dx * multiplier),
                                             .y = _context.position.y + (delta.dy * multiplier) });
        if (_fillSink)
        {
            _fillSink->push_back(next);
            _context.position = next;
        }
        else if (_collectingCurve)
        {
            // A pixel-vector run inside a C(S)/C(B)...(E) sequence contributes control points too,
            // mirroring parseCoordinateItem; without this branch the points would be silently dropped.
            _curvePoints.push_back(next);
            _context.position = next;
        }
        else if (command == Command::Vector)
            drawLineTo(next);
        else
            moveTo(next);
    }
}

void ReGISParser::parseFill()
{
    advance(); // '('
    skipWhitespace();
    // The fill region is defined by a nested V (polygon) or C (curve) sub-command.
    if (upper(peek()) == 'V' || upper(peek()) == 'C')
        advance();

    // The fill outline starts at the current graphics cursor, so it is the first vertex.
    auto vertices = std::vector<point> { _context.position };
    _fillSink = &vertices;
    while (!atEnd())
    {
        skipWhitespace();
        auto const c = peek();
        if (c == ')')
        {
            advance();
            break;
        }
        if (c == '[')
            parseCoordinateItem(Command::Vector);
        else if (isPvDigit(c))
            parsePixelVectorRun(Command::Vector);
        else if (c == '(')
            parseOptionSet(Command::Vector);
        else
            advance();
    }
    _fillSink = nullptr;

    if (vertices.size() >= 3)
    {
        _canvas.fillPolygon(_context.currentPen(), vertices);
        _context.position = vertices.back();
        _drew = true;
    }
}

// }}}

void ReGISParser::parseAll()
{
    auto command = Command::None;
    while (!atEnd())
    {
        skipWhitespace();
        if (atEnd())
            break;
        auto const c = peek();
        if (c == ';')
        {
            advance();
            command = Command::None;
            continue;
        }
        if (c == '(')
        {
            if (command == Command::Fill)
                parseFill();
            else
                parseOptionSet(command);
            continue;
        }
        if (c == '[')
        {
            parseCoordinateItem(command);
            continue;
        }
        if (c == '"' || c == '\'')
        {
            auto const text = scanQuotedString();
            if (command == Command::Text)
                drawText(text);
            continue;
        }
        if (isPvDigit(c))
        {
            parsePixelVectorRun(command);
            continue;
        }
        if (auto const letterCommand = commandOf(c); letterCommand != Command::None)
        {
            advance();
            command = letterCommand;
            _canvas.resetPattern();
            if (letterCommand == Command::Curve)
            {
                // The centre flag and arc sweep are per-C-command modifiers set by its options.
                _curveCenterMode = false;
                _curveArc.reset();
            }
            continue;
        }
        advance(); // skip an unrecognised byte (e.g. a stray digit 8/9)
    }
}

} // namespace vtbackend::regis
