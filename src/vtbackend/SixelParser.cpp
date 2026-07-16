// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/SixelParser.h>

#include <algorithm>
#include <array>
#include <bit>
#include <ranges>

using std::clamp;
using std::fill;
using std::max;
using std::min;
using std::vector;

// VT 340 sixel protocol is defined here: https://vt100.net/docs/vt3xx-gp/chapter14.html

using namespace std;
namespace vtbackend
{

namespace
{
    constexpr uint8_t toDigit(char value) noexcept
    {
        return static_cast<uint8_t>(value) - '0';
    }

    constexpr bool isSixel(char value) noexcept
    {
        return value >= 63 && value <= 126;
    }

    constexpr int8_t toSixel(char value) noexcept
    {
        return static_cast<int8_t>(static_cast<int>(value) - 63);
    }

    constexpr RGBColor rgb(uint8_t r, uint8_t g, uint8_t b)
    {
        return RGBColor { r, g, b };
    }

    constexpr double hue2rgb(double p, double q, double t) noexcept
    {
        if (t < 0)
            t += 1;
        if (t > 1)
            t -= 1;
        if (t < 1. / 6)
            return p + ((q - p) * 6 * t);
        if (t < 1. / 2)
            return q;
        if (t < 2. / 3)
            return p + ((q - p) * (2. / 3 - t) * 6);
        return p;
    }

    using NormalizedValue = double; // normalized value between [0, 1]

    constexpr RGBColor hsl2rgb(NormalizedValue h, NormalizedValue s, NormalizedValue l) noexcept
    {
        // See http://en.wikipedia.org/wiki/HSL_color_space.

        if (0 == s)
        {
            auto const grayscale = static_cast<uint8_t>(l * 255.);
            return RGBColor { grayscale, grayscale, grayscale };
        }
        else
        {
            auto const q = l < 0.5 ? l * (1 + s) : l + s - (l * s);
            auto const p = (2 * l) - q;

            auto result = RGBColor {};
            result.red = static_cast<uint8_t>(hue2rgb(p, q, h + (1. / 3)) * 255);
            result.green = static_cast<uint8_t>(hue2rgb(p, q, h) * 255);
            result.blue = static_cast<uint8_t>(hue2rgb(p, q, h - (1. / 3)) * 255);
            return result;
        }
    }

} // namespace

// VT 340 default color palette (https://www.vt100.net/docs/vt3xx-gp/chapter2.html#S2.4)
constexpr inline std::array<RGBColor, 16> DefaultColors = {
    rgb(0, 0, 0),       //  0: black
    rgb(51, 51, 204),   //  1: blue
    rgb(204, 33, 33),   //  2: red
    rgb(51, 204, 51),   //  3: green
    rgb(204, 51, 204),  //  4: magenta
    rgb(51, 204, 204),  //  5: cyan
    rgb(204, 204, 51),  //  6: yellow
    rgb(135, 135, 135), //  7: gray 50%
    rgb(66, 66, 66),    //  8: gray 25%
    rgb(84, 84, 153),   //  9: less saturated blue
    rgb(153, 66, 66),   // 10: less saturated red
    rgb(84, 153, 84),   // 11: less saturated green
    rgb(153, 84, 153),  // 12: less saturated magenta
    rgb(84, 153, 153),  // 13: less saturated cyan
    rgb(153, 153, 84),  // 14: less saturated yellow
    rgb(204, 204, 204), // 15: gray 75%
};

// {{{ SixelColorPalette
SixelColorPalette::SixelColorPalette(unsigned int size, unsigned int maxSize): _maxSize { maxSize }
{
    if (size > 0)
        _palette.resize(size);

    reset();
}

void SixelColorPalette::reset()
{
    for (size_t i = 0; i < min(static_cast<size_t>(size()), DefaultColors.size()); ++i)
        _palette[i] = DefaultColors[i];
}

void SixelColorPalette::setSize(unsigned int newSize)
{
    _palette.resize(static_cast<size_t>(max(0u, min(newSize, _maxSize))));
}

void SixelColorPalette::setColor(unsigned int index, RGBColor const& color)
{
    if (index < _maxSize)
    {
        if (index >= size())
            setSize(index + 1);

        if (static_cast<size_t>(index) < _palette.size())
            _palette.at(index) = color;
    }
}

RGBColor SixelColorPalette::at(unsigned int index) const noexcept
{
    // A palette may legitimately be empty: SixelColorPalette(0, max) only resizes when size > 0,
    // and the register count comes from a settings value.
    if (_palette.empty())
        return RGBColor {};
    return _palette[index % _palette.size()];
}
// }}}

SixelParser::SixelParser(Events& events, OnFinalize finalizer):
    _events { events }, _finalizer { std::move(finalizer) }
{
}

namespace
{
    /// The Sixel state machine. Built once at compile time; 5 states x 256 bytes x 2 matrices is
    /// ~2.5 KB, so the whole machine stays L1-resident.
    constexpr auto SixelTable = SixelParser::Table::get();
} // namespace

namespace
{
    /// @return true if @p action asks for no work at all.
    ///
    /// Most cells of a 5-state machine are inert -- Ground has neither entry nor exit action, and an
    /// introducer's transition carries none -- so testing here keeps the dispatcher from paying a
    /// call to reach a @c break.
    constexpr bool isInert(SixelParser::Action action) noexcept
    {
        return action == SixelParser::Action::Ignore || action == SixelParser::Action::Undefined;
    }
} // namespace

void SixelParser::parse(char value)
{
    auto const ch = static_cast<uint8_t>(value);
    auto const s = Table::index(_state);

    // The table is total -- every cell carries at least an action -- so there is no error branch.
    if (auto const next = SixelTable.transitions[s][ch]; next != State::Undefined)
    {
        if (auto const action = SixelTable.exitEvents[s]; !isInert(action))
            handle(action, ch);
        if (auto const action = SixelTable.events[s][ch]; !isInert(action))
            handle(action, ch);
        _state = next;
        if (auto const action = SixelTable.entryEvents[Table::index(next)]; !isInert(action))
            handle(action, ch);
    }
    else if (auto const action = SixelTable.events[s][ch]; !isInert(action))
        handle(action, ch);
}

void SixelParser::handle(Action action, uint8_t ch)
{
    switch (action)
    {
        case Action::Render: _events.render(toSixel(static_cast<char>(ch))); break;
        case Action::RenderRepeated:
            _events.renderRepeated(toSixel(static_cast<char>(ch)), _params[0]);
            break;
        case Action::Param: paramShiftAndAddDigit(toDigit(static_cast<char>(ch))); break;
        case Action::ParamSeparator: _params.push_back(0); break;
        case Action::Rewind: _events.rewind(); break;
        case Action::Newline: _events.newline(); break;
        case Action::ResetParams:
            _params.clear();
            _params.push_back(0);
            break;
        case Action::SubmitRaster: submitRaster(); break;
        case Action::SubmitColor: submitColor(); break;
        case Action::Ignore:
        case Action::Undefined: break;
    }
}

void SixelParser::done()
{
    // A sequence may end mid-introducer, so the current state's exit action still has to run: it is
    // what submits the parameters gathered so far, exactly as an aborting byte would have.
    handle(SixelTable.exitEvents[Table::index(_state)], 0);
    _state = State::Ground;
    _events.finalize();
    if (_finalizer)
        _finalizer();
}

void SixelParser::paramShiftAndAddDigit(unsigned value)
{
    unsigned& number = _params.back();
    number = number * 10 + value;
}

void SixelParser::submitRaster()
{
    // Fewer than two parameters says nothing, and more than four is not a raster attribute at all.
    if (!(_params.size() > 1 && _params.size() < 5))
        return;

    auto const pan = _params[0];
    auto const pad = _params[1];

    auto const imageSize = _params.size() > 3
                               ? optional<ImageSize> { ImageSize { Width(_params[2]), Height(_params[3]) } }
                               : std::nullopt;

    _events.setRaster(pan, pad, imageSize);
}

void SixelParser::submitColor()
{
    if (_params.size() == 1)
    {
        auto const index = _params[0];
        _events.useColor(index); // TODO: move color palette into image builder (to have access to it
                                 // during clear!)
    }
    else if (_params.size() == 5)
    {
        auto constexpr ConvertValue = [](unsigned value) {
            // converts a color from range 0..100 to 0..255
            return static_cast<uint8_t>(static_cast<int>((static_cast<float>(value) * 255.0f) / 100.0f)
                                        % 256);
        };
        auto const index = _params[0];
        auto const colorSpace = _params[1] == 2 ? Colorspace::RGB : Colorspace::HSL;
        switch (colorSpace)
        {
            case Colorspace::RGB: {
                auto const p1 = ConvertValue(_params[2]);
                auto const p2 = ConvertValue(_params[3]);
                auto const p3 = ConvertValue(_params[4]);
                auto const color = RGBColor { p1, p2, p3 }; // TODO: convert HSL if requested
                _events.setColor(index, color);
                break;
            }
            case Colorspace::HSL: {
                // HLS Values
                // Px 	0 to 360 degrees 	Hue angle
                // Py 	0 to 100 percent 	Lightness
                // Pz 	0 to 100 percent 	Saturation
                //
                // (Hue angle seems to be shifted by 120 deg in other Sixel implementations.)
                auto const h = static_cast<double>(_params[2]) - 120.0;
                auto const hc = (h < 0 ? 360 + h : h) / 360.0;
                auto const sc = static_cast<double>(_params[3]) / 100.0;
                auto const ls = static_cast<double>(_params[3]) / 100.0;
                auto const rgb = hsl2rgb(hc, sc, ls);
                _events.setColor(index, rgb);
                break;
            }
        }
        _events.useColor(index); // Also use the specified color.
    }
}

void SixelParser::pass(char ch)
{
    parse(ch);
}

void SixelParser::pass(std::string_view bytes)
{
    auto const* input = bytes.data();
    auto const* const end = input + bytes.size();

    while (input != end)
    {
        // In Ground state a sixel byte does nothing but render: parse() picks the Ground arm,
        // fallback() finds isSixel(), and the state does not move. So a run of them can go straight
        // to the sink, skipping a virtual call, a state switch and an introducer test per byte.
        // Pixel data is most of a sixel stream, so this is the path almost every byte takes.
        if (_state == State::Ground)
            while (input != end && isSixel(*input))
                _events.render(toSixel(*input++));

        if (input == end)
            break;

        parse(*input++);
    }
}

void SixelParser::finalize()
{
    done();
}

// =================================================================================

SixelImageBuilder::SixelImageBuilder(ImageSize maxSize,
                                     int aspectVertical,
                                     int aspectHorizontal,
                                     RGBAColor backgroundColor,
                                     std::shared_ptr<SixelColorPalette> colorPalette):
    _maxSize { maxSize },
    _colors { std::move(colorPalette) },
    _size { ImageSize { Width { 1 }, Height { 1 } } },
    _sixelCursor {},
    _aspectRatio(static_cast<unsigned int>(
        std::ceil(static_cast<float>(aspectVertical) / static_cast<float>(aspectHorizontal)))),
    _sixelBandHeight(6 * _aspectRatio)
{
    clear(backgroundColor);
    _currentColorValue = _colors->at(_currentColor);
}

void SixelImageBuilder::fillRun(std::span<uint8_t> dst, RGBAColor color) noexcept
{
    for (auto const pixel: std::views::iota(size_t { 0 }, dst.size() / 4))
    {
        auto* const p = dst.data() + (pixel * 4);
        p[0] = color.red();
        p[1] = color.green();
        p[2] = color.blue();
        p[3] = color.alpha();
    }
}

void SixelImageBuilder::clear(RGBAColor fillColor)
{
    _sixelCursor = {};
    _finalized = false;
    // Memoized so storage grown later is background-filled rather than zero-filled.
    _fillColor = fillColor;
    fillRun(_buffer, fillColor);
}

RGBAColor SixelImageBuilder::at(CellLocation coord) const noexcept
{
    auto const line = unbox(coord.line) % unbox(_size.height);
    auto const col = unbox(coord.column) % unbox(_size.width);
    auto const base = (line * _stride * 4) + (col * 4);
    const auto* const color = &_buffer[base];
    return RGBAColor { color[0], color[1], color[2], color[3] };
}

void SixelImageBuilder::write(CellLocation const& coord, RGBColor const& value) noexcept
{
    auto const canvas = canvasSize();
    if (unbox(coord.line) >= 0
        && unbox(coord.line) + static_cast<int>(_aspectRatio) <= unbox<int>(canvas.height)
        && unbox(coord.column) >= 0 && unbox(coord.column) < unbox<int>(canvas.width))
    {
        if (!_explicitSize)
        {
            if (unbox(coord.line) >= unbox<int>(_size.height))
                _size.height = Height::cast_from(std::min(coord.line.as<unsigned int>() + _aspectRatio,
                                                          unbox<unsigned int>(canvas.height)));
            if (unbox(coord.column) >= unbox<int>(_size.width))
                _size.width = Width::cast_from(coord.column + 1);
        }

        reserve(unbox<unsigned>(coord.column) + 1, coord.line.as<unsigned int>() + _aspectRatio);

        for (unsigned int i = 0; i < _aspectRatio; ++i)
        {
            auto const base = ((coord.line.as<unsigned int>() + i) * _stride * 4u)
                              + (unbox<unsigned int>(coord.column) * 4u);
            _buffer[base + 0] = value.red;
            _buffer[base + 1] = value.green;
            _buffer[base + 2] = value.blue;
            _buffer[base + 3] = 0xFF;
        }
    }
}

void SixelImageBuilder::setColor(unsigned index, RGBColor const& color)
{
    _colors->setColor(index, color);
    // Compare the raw index: useColor() already reduced _currentColor modulo the palette size, and
    // setColor() may have just grown the palette and changed that modulus. Redefining the register
    // currently in use must be visible without an intervening useColor().
    if (index == _currentColor)
        _currentColorValue = color;
}

void SixelImageBuilder::useColor(unsigned index)
{
    _currentColor = _colors->size() != 0 ? index % _colors->size() : 0;
    _currentColorValue = _colors->at(_currentColor);
}

void SixelImageBuilder::rewind()
{
    _sixelCursor.column = {};
}

void SixelImageBuilder::newline()
{
    _sixelCursor.column = {};
    if (unbox<unsigned int>(_sixelCursor.line) + _sixelBandHeight < unbox<unsigned int>(canvasSize().height))
        _sixelCursor.line = LineOffset::cast_from(_sixelCursor.line.as<unsigned int>() + _sixelBandHeight);
}

void SixelImageBuilder::setRaster(unsigned int pan, unsigned int pad, optional<ImageSize> imageSize)
{
    if (pad != 0)
        _aspectRatio =
            max(1u, static_cast<unsigned int>(std::ceil(static_cast<float>(pan) / static_cast<float>(pad))));
    _sixelBandHeight = 6 * _aspectRatio;
    if (imageSize)
    {
        imageSize->height = Height::cast_from(imageSize->height.value * _aspectRatio);
        _size.width = clamp(imageSize->width, Width(0), _maxSize.width);
        _size.height = clamp(imageSize->height, Height(0), _maxSize.height);
        _explicitSize = true;
        // Exactly the declared raster, background-filled: a plain resize() would zero-fill any
        // grown region, leaving a black band when a raster attribute widens the image.
        reshape(unbox<unsigned>(_size.width), unbox<unsigned>(_size.height));
        // A raster attribute redefines the image, so a previously finalized builder is live again.
        _finalized = false;
    }
}

void SixelImageBuilder::render(int8_t sixel)
{
    auto const canvas = canvasSize();
    auto const x = _sixelCursor.column.as<unsigned>();
    if (x >= unbox<unsigned>(canvas.width))
        return;

    auto const bits = static_cast<unsigned>(sixel) & SixelBitMask;

    // A blank sixel paints nothing, so it is only cursor movement -- and it dominates the stream.
    // Encoders emit one colour plane per pass, so on a 256-colour image roughly every column is
    // blank in all but one plane: measured on a plasma frame, 94% of all columns decoded are blank.
    if (bits == 0)
    {
        _sixelCursor.column++;
        return;
    }

    // Everything a write needs is the same for all six bits, so it is established once here rather
    // than re-derived per pixel: the column bound, the storage, and the colour.
    auto const y0 = _sixelCursor.line.as<unsigned>();
    auto const canvasHeight = unbox<unsigned>(canvas.height);

    // The last row this sixel actually reaches: the highest set bit whose rows all fit the canvas.
    // A bit that would overhang the bottom paints nothing and so cannot grow the image either --
    // taking the highest SET bit instead would grow it by the very rows that get clipped.
    auto const topBit = static_cast<unsigned>(std::bit_width(bits)) - 1;
    auto lastRowExclusive = y0 + ((topBit + 1) * _aspectRatio);
    if (lastRowExclusive > canvasHeight)
    {
        // Only when the sixel overhangs is it worth hunting for the highest bit that still fits.
        lastRowExclusive = 0;
        for (auto const bit: std::views::iota(0u, topBit + 1) | std::views::reverse)
        {
            if ((bits & (1u << bit)) == 0)
                continue;
            if (auto const y = y0 + (bit * _aspectRatio); y + _aspectRatio <= canvasHeight)
            {
                lastRowExclusive = y + _aspectRatio;
                break;
            }
        }
    }

    if (lastRowExclusive == 0)
    {
        _sixelCursor.column++; // every set bit overhangs the canvas
        return;
    }

    if (!_explicitSize)
    {
        if (lastRowExclusive > unbox<unsigned>(_size.height))
            _size.height = Height::cast_from(lastRowExclusive);
        if (x >= unbox<unsigned>(_size.width))
            _size.width = Width::cast_from(x + 1);
    }
    reserve(x + 1, lastRowExclusive);

    auto const color = currentColor();
    auto const rowBytes = static_cast<size_t>(_stride) * 4;

    // Walk the set bits rather than all six: a column of a colour plane usually carries one or two.
    for (auto remaining = bits; remaining != 0; remaining &= remaining - 1)
    {
        auto const bit = static_cast<unsigned>(std::countr_zero(remaining));
        auto const y = y0 + (bit * _aspectRatio);
        if (y + _aspectRatio > canvasHeight)
            break; // bits only climb, so nothing above this fits either

        auto* pixel = _buffer.data() + (y * rowBytes) + (static_cast<size_t>(x) * 4);
        pixel[0] = color.red;
        pixel[1] = color.green;
        pixel[2] = color.blue;
        pixel[3] = 0xFF;

        // Aspect ratio 1 is the norm and means one pixel row per bit; only a stretched image
        // repeats, and paying a loop for the common case cost more than the store it guarded.
        for ([[maybe_unused]] auto const row: std::views::iota(1u, _aspectRatio))
        {
            pixel += rowBytes;
            pixel[0] = color.red;
            pixel[1] = color.green;
            pixel[2] = color.blue;
            pixel[3] = 0xFF;
        }
    }

    _sixelCursor.column++;
}

void SixelImageBuilder::renderRepeated(int8_t sixel, unsigned count)
{
    // render() is a no-op once the cursor has reached the canvas edge, so repetitions beyond it
    // cannot change anything -- and the count is attacker-controlled: '!4294967295~' would
    // otherwise spin through four billion calls that each do nothing but fail a bounds check.
    auto const canvasWidth = unbox<unsigned>(canvasSize().width);
    auto const cursorX = _sixelCursor.column.as<unsigned>();
    if (cursorX >= canvasWidth)
        return;

    auto const run = std::min(count, canvasWidth - cursorX);

    // A run of blanks paints nothing at all, so it is exactly a cursor advance -- no need to walk
    // it. This is the common case by a wide margin: encoders emit one colour plane per pass, so a
    // 256-colour image is mostly '!<n>?' runs, and n can be a whole image width.
    if ((static_cast<unsigned>(sixel) & SixelBitMask) == 0)
    {
        _sixelCursor.column = ColumnOffset::cast_from(cursorX + run);
        return;
    }

    for ([[maybe_unused]] auto const repetition: std::views::iota(0u, run))
        render(sixel);
}

void SixelImageBuilder::finalize()
{
    if (_finalized)
        return;
    _finalized = true;

    if (unbox(_size.height) == 1)
    {
        _size.height = Height::cast_from(_sixelCursor.line.as<unsigned int>() * _aspectRatio);
        reshape(unbox<unsigned>(_size.width), unbox<unsigned>(_size.height));
        return;
    }
    if (!_explicitSize)
    {
        reshape(unbox<unsigned>(_size.width), unbox<unsigned>(_size.height));
        _explicitSize = true;
    }
}

void SixelImageBuilder::reshape(unsigned newStride, unsigned newRows)
{
    if (newStride == _stride && newRows == _allocatedHeight)
        return;

    auto tempBuffer = Buffer(static_cast<size_t>(newStride) * newRows * 4);
    auto const rowsToCopy = std::min(newRows, _allocatedHeight);
    auto const pixelsPerRow = std::min(newStride, _stride);
    auto const rowBytes = static_cast<size_t>(newStride) * 4;

    // Every byte is written at most once: carried-over pixels are copied, and only genuinely new
    // area is background-filled. Filling the whole buffer first would re-do the copied region --
    // and background, not std::vector's zero-fill, is what newly exposed image area must show.
    for (auto const row: std::views::iota(0u, rowsToCopy))
    {
        auto* const dst = tempBuffer.data() + (row * rowBytes);
        std::copy_n(_buffer.data() + (static_cast<size_t>(row) * _stride * 4),
                    static_cast<size_t>(pixelsPerRow) * 4,
                    dst);
        if (pixelsPerRow < newStride)
            fillRun({ dst + (static_cast<size_t>(pixelsPerRow) * 4),
                      static_cast<size_t>(newStride - pixelsPerRow) * 4 },
                    _fillColor);
    }

    if (rowsToCopy < newRows)
        fillRun({ tempBuffer.data() + (rowsToCopy * rowBytes), (newRows - rowsToCopy) * rowBytes },
                _fillColor);

    _buffer.swap(tempBuffer);
    _stride = newStride;
    _allocatedHeight = newRows;
}

namespace
{
    /// Rounds @p required up to the next geometric growth step, capped at @p limit.
    constexpr unsigned grownTo(unsigned current, unsigned required, unsigned limit) noexcept
    {
        auto constexpr MinGrowth = 16u;
        auto value = std::max(current, MinGrowth);
        while (value < required)
            value *= 2;
        return std::min(value, limit);
    }
} // namespace

void SixelImageBuilder::reserve(unsigned columns, unsigned rows)
{
    if (columns <= _stride && rows <= _allocatedHeight)
        return;

    auto const canvas = canvasSize();
    reshape(std::max(_stride, grownTo(_stride, columns, unbox<unsigned>(canvas.width))),
            std::max(_allocatedHeight, grownTo(_allocatedHeight, rows, unbox<unsigned>(canvas.height))));
}

} // namespace vtbackend
