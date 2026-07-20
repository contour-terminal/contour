// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/SixelParser.h>

#include <algorithm>
#include <array>
#include <bit>
#include <ranges>

using std::clamp;
using std::max;
using std::min;
using std::vector;

// VT 340 sixel protocol is defined here: https://vt100.net/docs/vt3xx-gp/chapter14.html

using namespace std;
namespace vtbackend
{

namespace
{
    constexpr bool isDigit(char value) noexcept
    {
        return value >= '0' && value <= '9';
    }

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

} // namespace

// The VT340 default palette and the HLS->RGB conversion live in vtbackend/Color.h, shared verbatim
// with the ReGIS colour introducer (@ref vtbackend::VT340DefaultColorPalette, @ref decHlsToRgb).

// {{{ SixelColorPalette
SixelColorPalette::SixelColorPalette(unsigned int size, unsigned int maxSize): _maxSize { maxSize }
{
    if (size > 0)
        _palette.resize(size);

    reset();
}

void SixelColorPalette::reset()
{
    for (size_t i = 0; i < min(static_cast<size_t>(size()), VT340DefaultColorPalette.size()); ++i)
        _palette[i] = VT340DefaultColorPalette[i];
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

    /// @return true if @p action asks for no work at all.
    ///
    /// Most cells of a 5-state machine are inert -- Ground has neither entry nor exit action, and an
    /// introducer's transition carries none -- so testing here keeps the dispatcher from paying a
    /// call to reach a @c break.
    constexpr bool isInert(SixelParser::Action action) noexcept
    {
        return action == SixelParser::Action::Ignore || action == SixelParser::Action::Undefined;
    }

    /// @return true if every state treats all ten digits identically.
    ///
    /// What licenses the digit-run fast path in parse(): it consults the table for the FIRST digit of a
    /// run and then scans the rest by isDigit() alone, which is only sound while '0'..'9' share one cell
    /// in every state. A table change that gave one digit its own transition would silently make that
    /// run take the wrong branch, so it is asserted here rather than left as a comment.
    consteval bool theHotRunsAreFoldable() noexcept
    {
        auto const table = SixelParser::Table::get();
        return std::ranges::all_of(std::views::iota(size_t { 0 }, SixelParser::StateCount), [&](size_t s) {
            return std::ranges::all_of(std::views::iota(uint8_t { '1' }, uint8_t { '9' + 1 }),
                                       [&](uint8_t d) {
                                           return table.transitions[s][d] == table.transitions[s]['0']
                                                  && table.events[s][d] == table.events[s]['0'];
                                       });
        });
    }
    static_assert(theHotRunsAreFoldable(), "The digit-run fast path requires '0'..'9' to share a cell.");
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
        case Action::ParamSeparator:
            // Saturates rather than grows: the new slot is zeroed either way, so a stream past the
            // sixth parameter keeps overwriting the spare one and no reader can tell.
            _paramCount = std::min(_paramCount + 1, _params.size());
            _params[_paramCount - 1] = 0;
            break;
        case Action::Rewind: _events.rewind(); break;
        case Action::Newline: _events.newline(); break;
        case Action::ResetParams:
            _paramCount = 1;
            _params[0] = 0;
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
    // _paramCount is never 0 here: a digit is only ever reachable from a state whose entry action
    // opened the list, and the count saturates one slot short of the array's end.
    unsigned& number = _params[_paramCount - 1];
    number = (number * 10) + value;
}

void SixelParser::foldDigits(std::string_view digits)
{
    unsigned& number = _params[_paramCount - 1];
    auto value = number;
    for (auto const ch: digits)
        value = (value * 10) + toDigit(ch);
    number = value;
}

void SixelParser::submitRaster()
{
    // Fewer than two parameters says nothing, and more than four is not a raster attribute at all.
    if (!(_paramCount > 1 && _paramCount < 5))
        return;

    auto const pan = _params[0];
    auto const pad = _params[1];

    auto const imageSize = _paramCount > 3
                               ? optional<ImageSize> { ImageSize { Width(_params[2]), Height(_params[3]) } }
                               : std::nullopt;

    _events.setRaster(pan, pad, imageSize);
}

void SixelParser::submitColor()
{
    if (_paramCount == 1)
    {
        auto const index = _params[0];
        _events.useColor(index); // TODO: move color palette into image builder (to have access to it
                                 // during clear!)
    }
    else if (_paramCount == 5)
    {
        auto constexpr ConvertValue = [](unsigned value) {
            // Converts a color from range 0..100 to 0..255, saturating at full intensity.
            //
            // The parameter comes straight off the wire unclamped, so '#0;2;99999999999999999999;0;0'
            // reaches here as a wrapped value near 2^32: scaling that as a float lands outside int's
            // range, and the conversion is undefined before the old '% 256' ever got to tame it.
            // Integer math over a saturated value cannot overflow -- 100 * 255 is 25500 -- and 0..100
            // is the only range the VT340 defines, so anything above it is full intensity.
            return static_cast<uint8_t>((std::min(value, 100u) * 255u) / 100u);
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
                //
                // decHlsToRgb saturates each parameter to its VT340 range first: they arrive off the
                // wire unclamped, and converting an out-of-range double to uint8_t is undefined. The
                // conversion is shared with the ReGIS colour introducer so both render identically.
                auto const rgb = decHlsToRgb(_params[2], _params[3], _params[4]);
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
        // In Ground state a sixel byte does nothing but render: the table names Render and does not
        // move. So a run of them goes straight to the sink, skipping the table lookup and the
        // dispatch per byte -- and, more importantly, reaching the sink as a run rather than as N
        // separate calls, so the sink can establish once what every column of it shares.
        if (_state == State::Ground)
        {
            auto const* const runBegin = input;
            while (input != end && isSixel(*input))
                ++input;
            if (input != runBegin)
                _events.renderRun(std::string_view { runBegin, static_cast<size_t>(input - runBegin) });
        }
        else if (auto const s = Table::index(_state);
                 isDigit(*input)
                 && SixelTable.transitions[s][static_cast<uint8_t>(*input)] == State::Undefined)
        {
            // The table says a digit here stays put and folds into the current parameter. That is
            // true of every parameter state but not of ColorIntroducer, whose first digit opens
            // ColorParam -- so the branch is gated on the table rather than on a list of states
            // spelled out here. Scanning the rest of the run by isDigit() alone is sound because all
            // ten digits share that one cell, which theHotRunsAreFoldable() asserts at compile time.
            //
            // Digits are 30% of a sixel stream and each one otherwise pays a full table dispatch to
            // do n = n*10 + d.
            auto const* const runBegin = input;
            do
                ++input;
            while (input != end && isDigit(*input));
            foldDigits(std::string_view { runBegin, static_cast<size_t>(input - runBegin) });
            continue;
        }

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
    // Bounded by the storage rather than by _size: the two agree only once something has painted.
    // A stream that paints nothing leaves _size at its constructed 1x1 sentinel with no buffer
    // behind it, and a raster declaring a zero dimension leaves the geometry at zero -- neither may
    // be indexed, and neither may be a modulus. For any pixel inside the image the two moduli agree,
    // because reserve() backs every paint and _stride is the row pitch _buffer is actually laid out
    // in.
    if (_stride == 0 || _allocatedHeight == 0)
        return _fillColor;

    auto const line = static_cast<size_t>(coord.line.as<unsigned>() % _allocatedHeight);
    auto const col = static_cast<size_t>(coord.column.as<unsigned>() % _stride);
    auto const base = ((line * _stride) + col) * 4;
    auto const* const color = &_buffer[base];
    return RGBAColor { color[0], color[1], color[2], color[3] };
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

        paintBit(_buffer.data() + (y * rowBytes) + (static_cast<size_t>(x) * 4), rowBytes, color);
    }

    _sixelCursor.column++;
}

void SixelImageBuilder::paintBit(uint8_t* pixel, size_t rowBytes, RGBColor color) const noexcept
{
    pixel[0] = color.red;
    pixel[1] = color.green;
    pixel[2] = color.blue;
    pixel[3] = 0xFF;

    // Aspect ratio 1 is the norm and means one pixel row per bit; only a stretched image repeats,
    // and paying a loop for the common case cost more than the store it guarded.
    for ([[maybe_unused]] auto const row: std::views::iota(1u, _aspectRatio))
    {
        pixel += rowBytes;
        pixel[0] = color.red;
        pixel[1] = color.green;
        pixel[2] = color.blue;
        pixel[3] = 0xFF;
    }
}

void SixelImageBuilder::renderRun(std::string_view sixels)
{
    // Without an explicit raster the image grows as it is painted, so the canvas a column may write
    // into depends on what the column before it did -- nothing below would be loop-invariant. Every
    // real encoder declares a raster (img2sixel, chafa and termbench-pro all emit '"Pan;Pad;Ph;Pv'),
    // so the growth path keeps the per-column code and stays the simple one.
    if (!_explicitSize)
    {
        Events::renderRun(sixels);
        return;
    }

    auto const canvas = canvasSize();
    auto const canvasWidth = unbox<unsigned>(canvas.width);
    auto x = _sixelCursor.column.as<unsigned>();
    if (x >= canvasWidth)
        return; // render() paints nothing and does not advance once the cursor is off the canvas

    auto const run = std::min(static_cast<unsigned>(sixels.size()), canvasWidth - x);

    // Everything from here to the loop is what render() re-established on every single column, and
    // none of it can change within a run: the cursor's line, the canvas, the colour and the buffer's
    // geometry are all fixed until a '-', '$', '#' or '!' ends the run -- and any of those would
    // have ended it before this call. On a real frame that setup measured 9.8 instructions per
    // painted column against 2.7 for the pixel stores it guarded.
    auto const color = currentColor();
    auto const rowBytes = static_cast<size_t>(_stride) * 4;
    // An explicit raster is exactly what storage covers -- setRaster() reshapes to it -- so the
    // reserve() render() calls here can only ever early-return.
    auto* const base = _buffer.data();
    auto const band = bandRows();

    for (auto const ch: sixels.substr(0, run))
    {
        auto const bits = static_cast<unsigned>(static_cast<int>(ch) - 63) & SixelBitMask & band.fittingBits;
        for (auto remaining = bits; remaining != 0; remaining &= remaining - 1)
        {
            auto const bit = static_cast<unsigned>(std::countr_zero(remaining));
            paintBit(base + band.rowOffsets[bit] + (static_cast<size_t>(x) * 4), rowBytes, color);
        }
        ++x;
    }

    _sixelCursor.column = ColumnOffset::cast_from(x);
}

SixelImageBuilder::BandRows SixelImageBuilder::bandRows() const noexcept
{
    auto result = BandRows {};
    auto const canvasHeight = unbox<unsigned>(canvasSize().height);
    auto const y0 = _sixelCursor.line.as<unsigned>();
    auto const rowBytes = static_cast<size_t>(_stride) * 4;

    for (auto const bit: std::views::iota(0u, SixelBitCount))
    {
        auto const y = y0 + (bit * _aspectRatio);
        if (y + _aspectRatio > canvasHeight)
            break; // bits only climb, so nothing above this fits either
        result.fittingBits |= 1u << bit;
        result.rowOffsets[bit] = static_cast<size_t>(y) * rowBytes;
    }
    return result;
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
    auto const sixelBits = static_cast<unsigned>(sixel) & SixelBitMask;
    if (sixelBits == 0)
    {
        _sixelCursor.column = ColumnOffset::cast_from(cursorX + run);
        return;
    }

    // Without an explicit raster each repetition may grow the image, so what one column writes
    // depends on the one before it and none of the below holds. See renderRun().
    if (!_explicitSize)
    {
        for ([[maybe_unused]] auto const repetition: std::views::iota(0u, run))
            render(sixel);
        return;
    }

    // A repeat is the same bits in every column of the run, so a set bit is one *horizontal* run of
    // pixels rather than `run` separate four-byte stores at a stride. That is a different shape from
    // renderRun(), which walks distinct bytes: here the buffer is written contiguously, one fill per
    // set bit, instead of scattered per column.
    auto const color = currentColor();
    auto const fill = RGBAColor { color.red, color.green, color.blue, 0xFF };
    auto const rowBytes = static_cast<size_t>(_stride) * 4;
    auto const runBytes = static_cast<size_t>(run) * 4;
    auto* const base = _buffer.data();
    auto const band = bandRows();

    for (auto remaining = sixelBits & band.fittingBits; remaining != 0; remaining &= remaining - 1)
    {
        auto const bit = static_cast<unsigned>(std::countr_zero(remaining));
        auto* dst = base + band.rowOffsets[bit] + (static_cast<size_t>(cursorX) * 4);
        // Aspect ratio 1 is the norm: one pixel row per bit, so one fill.
        for ([[maybe_unused]] auto const row: std::views::iota(0u, _aspectRatio))
        {
            fillRun({ dst, runBytes }, fill);
            dst += rowBytes;
        }
    }

    _sixelCursor.column = ColumnOffset::cast_from(cursorX + run);
}

void SixelImageBuilder::finalize()
{
    if (_finalized)
        return;
    _finalized = true;

    // Nothing ever painted and no raster declared a size, so the image is only what the cursor
    // walked over. Asking the storage is what tells that apart: reserve() backs every paint, so an
    // empty buffer means nothing landed. Testing _size.height against its constructed 1 sentinel
    // cannot -- a single painted pixel row is a height of 1 too, and an explicit `"1;1;Ph;1` raster
    // declares one, so both were compacted away to a zero-height image.
    if (!_explicitSize && _allocatedHeight == 0)
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
