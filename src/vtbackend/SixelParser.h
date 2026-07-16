// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/primitives.h>

#include <vtparser/ParserExtension.h>

#include <crispy/range.h>

#include <array>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// Bits one sixel character carries; each is one pixel row of the band.
constexpr inline unsigned SixelBitCount = 6;

/// Mask of the meaningful bits of a decoded sixel. Zero means the column paints nothing.
constexpr inline unsigned SixelBitMask = (1u << SixelBitCount) - 1u;

/// VT340 vertical aspect ratio (Pan) by sixel DCS parameter P1.
/// @see https://vt100.net/docs/vt3xx-gp/chapter14.html
constexpr inline std::array<int, 10> SixelAspectVerticalByP1 = { 2, 2, 5, 3, 3, 2, 2, 1, 1, 1 };

/// Maps a sixel DCS P1 parameter to its vertical aspect ratio.
/// @param p1 the DCS P1 parameter.
/// @return the vertical aspect ratio; 1 for values outside the defined range.
[[nodiscard]] constexpr int sixelAspectVertical(unsigned p1) noexcept
{
    return p1 < SixelAspectVerticalByP1.size() ? SixelAspectVerticalByP1[p1] : 1;
}

/// Sixel Stream Parser API.
///
/// Parses a sixel stream without any Sixel introducer CSI or ST to leave sixel mode,
/// that must be done by the parent parser.
class SixelParser: public ParserExtension
{
  public:
    /// Sixel parser states.
    ///
    /// @c Undefined is the state table's "no entry" sentinel and must remain the first enumerator:
    /// a zero-initialized table then means "nothing defined here".
    enum class State : uint8_t
    {
        Undefined = 0,    // table sentinel, never entered
        Ground,           // Sixel data
        RasterSettings,   // '"', configuring the raster
        RepeatIntroducer, // '!'
        ColorIntroducer,  // '#', color-set or color-use
        ColorParam        // color parameter
    };

    /// Number of @c State enumerators, including the @c Undefined sentinel. Sizes the state table.
    static constexpr size_t StateCount = 6;
    static_assert(static_cast<size_t>(State::ColorParam) + 1 == StateCount,
                  "StateCount must cover every State enumerator");

    /// The work a table cell asks the dispatcher to perform.
    ///
    /// @c Undefined is the "no entry" sentinel and must remain the first enumerator.
    enum class Action : uint8_t
    {
        Undefined = 0,  ///< Table sentinel: nothing defined.
        Ignore,         ///< Consume the byte and do nothing.
        Render,         ///< Paint the byte's sixel at the cursor.
        RenderRepeated, ///< Paint the byte's sixel as many times as the first parameter says.
        Param,          ///< Fold the byte's digit into the current parameter.
        ParamSeparator, ///< Begin a new parameter.
        Rewind,         ///< Move the sixel cursor back to the left margin.
        Newline,        ///< Move the sixel cursor to the next band's left margin.
        ResetParams,    ///< Entry action: begin a fresh parameter list.
        SubmitRaster,   ///< Exit action: hand the gathered raster attributes to the sink.
        SubmitColor,    ///< Exit action: define and/or select a colour register.
    };

    /// The Sixel state machine, expressed as data.
    ///
    /// Two byte-matrices indexed by @c [state][byte] -- one naming the state to move to, one naming
    /// the action to perform -- plus one entry and one exit action per state. @c Undefined (0) means
    /// "no entry", so the dispatcher tells a transition from an in-state event by testing
    /// @c transitions[s][ch] against it. The table is total: every cell carries at least an action,
    /// so the dispatcher needs no error branch.
    ///
    /// Modelled on vtparser's @c ParserTable (src/vtparser/Parser-impl.h) with one deliberate
    /// difference: the action arrays are sized by @c State, which is what indexes them. vtparser
    /// sizes its own by @c Action and is saved only by happening to have more actions than states.
    struct Table
    {
        /// An inclusive byte range, so one builder call covers a whole class of input.
        struct Range
        {
            uint8_t first;
            uint8_t last;
        };

        /// State to move to for a given (state, byte), or @c Undefined to stay put.
        std::array<std::array<State, 256>, StateCount> transitions {};
        /// Action to perform for a given (state, byte).
        std::array<std::array<Action, 256>, StateCount> events {};
        /// Action to perform on entering a state.
        std::array<Action, StateCount> entryEvents {};
        /// Action to perform on leaving a state.
        std::array<Action, StateCount> exitEvents {};

        /// Builds the Sixel state machine table.
        [[nodiscard]] static constexpr Table get() noexcept;

        /// @return @p state as an index into the table's rows.
        [[nodiscard]] static constexpr size_t index(State state) noexcept
        {
            return static_cast<size_t>(state);
        }

        constexpr void entry(State state, Action action) noexcept { entryEvents[index(state)] = action; }
        constexpr void exit(State state, Action action) noexcept { exitEvents[index(state)] = action; }

        /// Handles @p input in place: performs @p action without moving state.
        constexpr void event(State state, Action action, uint8_t input) noexcept
        {
            events[index(state)][input] = action;
            // An in-state event does not move, so it must also retract any transition a broader
            // earlier rule (such as the catch-all default) had placed on this byte.
            transitions[index(state)][input] = State::Undefined;
        }

        constexpr void event(State state, Action action, Range input) noexcept
        {
            for (auto const ch: std::views::iota(unsigned { input.first }, unsigned { input.last } + 1u))
                event(state, action, static_cast<uint8_t>(ch));
        }

        /// Moves from @p from to @p to on @p input, performing @p action between the two states'
        /// exit and entry actions.
        constexpr void transition(State from, State to, Action action, uint8_t input) noexcept
        {
            event(from, action, input);
            transitions[index(from)][input] = to;
        }

        constexpr void transition(State from, State to, Action action, Range input) noexcept
        {
            for (auto const ch: std::views::iota(unsigned { input.first }, unsigned { input.last } + 1u))
                transition(from, to, action, static_cast<uint8_t>(ch));
        }
    };

    enum class Colorspace : uint8_t
    {
        RGB,
        HSL
    };

    /// SixelParser's event handler
    class Events
    {
      public:
        virtual ~Events() = default;

        /// Defines a new color at given register index.
        virtual void setColor(unsigned index, RGBColor const& color) = 0;

        /// Uses the given color for future paints
        virtual void useColor(unsigned index) = 0;

        /// moves sixel-cursor to the left border
        virtual void rewind() = 0;

        /// moves the sixel-cursorto the left border of the next sixel-band
        virtual void newline() = 0;

        /// Defines the aspect ratio (pan / pad = aspect ratio) and image dimensions in pixels for
        /// the upcoming pixel data.
        virtual void setRaster(unsigned int pan, unsigned int pad, std::optional<ImageSize> imageSize) = 0;

        /// renders a given sixel at the current sixel-cursor position.
        virtual void render(int8_t sixel) = 0;

        /// Renders @p sixel @p count times horizontally, starting at the sixel-cursor.
        ///
        /// The default implementation simply loops over render(). Implementers that know their
        /// canvas bounds should override it to skip repetitions that would fall outside: the
        /// repeat count comes straight off the wire, so an untrusted stream can otherwise ask for
        /// billions of no-op calls.
        /// @param sixel the six-bit vertical pixel pattern.
        /// @param count number of horizontal repetitions; 0 renders nothing.
        virtual void renderRepeated(int8_t sixel, unsigned count)
        {
            for ([[maybe_unused]] auto const repetition: std::views::iota(0u, count))
                render(sixel);
        }

        /// Finalizes the image by optimizing the underlying storage to its minimal dimension in storage.
        virtual void finalize() = 0;
    };

    using OnFinalize = std::function<void()>;
    explicit SixelParser(Events& events, OnFinalize finalizer = {});

    using iterator = char const*;

    void parseFragment(iterator begin, iterator end)
    {
        for (auto const ch: crispy::range(begin, end))
            parse(ch);
    }

    void parseFragment(std::string_view range) { parseFragment(range.data(), range.data() + range.size()); }

    void parse(char value);
    void done();

    static void parse(std::string_view range, Events& events)
    {
        auto parser = SixelParser { events };
        parser.parseFragment(range.data(), range.data() + range.size());
        parser.done();
    }

    // ParserExtension overrides
    void pass(char ch) override;

    /// Consumes a whole run of sixel bytes.
    ///
    /// Overridden rather than left to the base's per-byte loop: pixel data is most of a sixel
    /// stream and needs no dispatch at all, so the run is consumed directly.
    /// @param bytes The payload bytes.
    void pass(std::string_view bytes) override;
    void finalize() override;

  private:
    /// Performs @p action for the byte @p ch.
    /// @param action the action a table cell named; @c Ignore and @c Undefined do nothing.
    /// @param ch the input byte the action applies to.
    void handle(Action action, uint8_t ch);

    void paramShiftAndAddDigit(unsigned value);

    /// Hands the gathered raster attributes to the sink, if they form a well-shaped set.
    void submitRaster();

    /// Defines and/or selects a colour register from the gathered parameters.
    void submitColor();

  private:
    State _state = State::Ground;
    std::vector<unsigned> _params;

    Events& _events;
    OnFinalize _finalizer;
};

constexpr SixelParser::Table SixelParser::Table::get() noexcept
{
    auto t = Table {};

    auto constexpr AllBytes = Range { .first = 0x00, .last = 0xFF };
    auto constexpr Digits = Range { .first = '0', .last = '9' };
    // A sixel byte carries six pixel rows biased by 63; 63..126 is the whole of the data alphabet,
    // and every punctuation byte below names an introducer well under 63. The two cannot overlap,
    // which is why the rules below may be written in any order.
    auto constexpr Sixels = Range { .first = 63, .last = 126 };

    // The catch-all, laid down first so the specific rules below overwrite it. In Ground an unnamed
    // byte is simply ignored; anywhere else it aborts back to Ground -- firing that state's exit
    // action, which is how a half-finished introducer still submits what it gathered.
    for (auto const s: std::views::iota(size_t { 1 }, StateCount))
    {
        auto const state = static_cast<State>(s);
        if (state == State::Ground)
            t.event(state, Action::Ignore, AllBytes);
        else
            t.transition(state, State::Ground, Action::Ignore, AllBytes);
    }

    // Pixel data. In Ground it paints and stays; anywhere else it first aborts the pending
    // introducer. Expressing this as table rows is what retires the old fallback()'s hand-tuned
    // "test isSixel before the five introducers" ordering: a lookup has no order to tune.
    t.event(State::Ground, Action::Render, Sixels);
    t.transition(State::RasterSettings, State::Ground, Action::Render, Sixels);
    t.transition(State::ColorIntroducer, State::Ground, Action::Render, Sixels);
    t.transition(State::ColorParam, State::Ground, Action::Render, Sixels);
    // '!' NUMBER BYTE -- the byte both closes the run and says what to paint.
    t.transition(State::RepeatIntroducer, State::Ground, Action::RenderRepeated, Sixels);

    // The introducers, reachable from every state including themselves.
    for (auto const s: std::views::iota(size_t { 1 }, StateCount))
    {
        auto const state = static_cast<State>(s);
        t.transition(state, State::ColorIntroducer, Action::Ignore, uint8_t { '#' });
        t.transition(state, State::RepeatIntroducer, Action::Ignore, uint8_t { '!' });
        t.transition(state, State::RasterSettings, Action::Ignore, uint8_t { '"' });
        t.transition(state, State::Ground, Action::Rewind, uint8_t { '$' });
        t.transition(state, State::Ground, Action::Newline, uint8_t { '-' });
    }

    // Parameters.
    t.event(State::RepeatIntroducer, Action::Param, Digits);
    t.event(State::RasterSettings, Action::Param, Digits);
    t.event(State::RasterSettings, Action::ParamSeparator, uint8_t { ';' });
    t.event(State::ColorParam, Action::Param, Digits);
    t.event(State::ColorParam, Action::ParamSeparator, uint8_t { ';' });
    // A '#' introducer's first digit both opens the parameter and enters ColorParam.
    t.transition(State::ColorIntroducer, State::ColorParam, Action::Param, Digits);

    // Each introducer starts a fresh parameter list. ColorParam deliberately has no entry action:
    // it inherits the list ColorIntroducer opened.
    t.entry(State::RasterSettings, Action::ResetParams);
    t.entry(State::RepeatIntroducer, Action::ResetParams);
    t.entry(State::ColorIntroducer, Action::ResetParams);

    // Parameters only mean something once whatever carried them ends.
    t.exit(State::RasterSettings, Action::SubmitRaster);
    t.exit(State::ColorParam, Action::SubmitColor);

    return t;
}

class SixelColorPalette
{
  public:
    SixelColorPalette(unsigned int size, unsigned int maxSize);

    void reset();

    [[nodiscard]] unsigned int size() const noexcept { return static_cast<unsigned int>(_palette.size()); }
    void setSize(unsigned int newSize);

    [[nodiscard]] unsigned int maxSize() const noexcept { return _maxSize; }
    void setMaxSize(unsigned int value) { _maxSize = value; }

    void setColor(unsigned int index, RGBColor const& color);
    [[nodiscard]] RGBColor at(unsigned int index) const noexcept;

  private:
    std::vector<RGBColor> _palette;
    unsigned int _maxSize;
};

/// Sixel Image Builder API
///
/// Implements the SixelParser::Events event listener to construct a Sixel image.
class SixelImageBuilder: public SixelParser::Events
{
  public:
    using Buffer = std::vector<uint8_t>;

    SixelImageBuilder(ImageSize maxSize,
                      int aspectVertical,
                      int aspectHorizontal,
                      RGBAColor backgroundColor,
                      std::shared_ptr<SixelColorPalette> colorPalette);

    [[nodiscard]] ImageSize maxSize() const noexcept { return _maxSize; }
    [[nodiscard]] ImageSize size() const noexcept { return _size; }

    /// The bounds every pixel write is clamped against.
    ///
    /// Once a raster attribute has declared an explicit size, that size *is* the canvas and nothing
    /// may be written outside it. Without one the image grows on demand, so the canvas is the
    /// maximum permitted image size. This is the single authority for "may I write here?" — the
    /// pixel buffer's own geometry always follows it, never the other way round.
    [[nodiscard]] ImageSize canvasSize() const noexcept { return _explicitSize ? _size : _maxSize; }
    [[nodiscard]] unsigned int aspectRatio() const noexcept { return _aspectRatio; }

    /// The color pixels are currently painted with.
    ///
    /// Resolved once in setColor()/useColor() rather than per pixel: this sits in the innermost
    /// loop, where looking it up cost a shared_ptr dereference, a size() load and an integer
    /// division for a value that only changes on a '#'.
    [[nodiscard]] RGBColor currentColor() const noexcept { return _currentColorValue; }

    [[nodiscard]] RGBAColor at(CellLocation coord) const noexcept;

    [[nodiscard]] Buffer const& data() const noexcept { return _buffer; }
    [[nodiscard]] Buffer& data() noexcept { return _buffer; }

    void clear(RGBAColor fillColor);

    void setColor(unsigned index, RGBColor const& color) override;
    void useColor(unsigned index) override;
    void rewind() override;
    void newline() override;
    void setRaster(unsigned int pan, unsigned int pad, std::optional<ImageSize> imageSize) override;
    void render(int8_t sixel) override;
    void renderRepeated(int8_t sixel, unsigned count) override;
    void finalize() override;

    [[nodiscard]] CellLocation const& sixelCursor() const noexcept { return _sixelCursor; }

  private:
    void write(CellLocation const& coord, RGBColor const& value) noexcept;

    /// Re-lays the pixel buffer out to @p newStride pixels per row and @p newRows rows.
    ///
    /// Overlapping pixel content is preserved. This is the only function that moves pixels, so the
    /// buffer's geometry can only ever change through it.
    /// @param newStride pixels per row of the new layout.
    /// @param newRows number of rows the new layout must back.
    void reshape(unsigned newStride, unsigned newRows);

    /// Ensures the buffer physically backs pixel (@p columns - 1, @p rows - 1), growing it
    /// geometrically if not.
    ///
    /// On the explicit-raster path this always early-returns: the write guard has already
    /// established that the pixel lies inside the raster, which storage exactly covers.
    void reserve(unsigned columns, unsigned rows);

    /// Fills @p dst, a run of consecutive RGBA pixels, with @p color.
    /// @param dst destination run; its size must be a multiple of 4.
    /// @param color the color to write.
    static void fillRun(std::span<uint8_t> dst, RGBAColor color) noexcept;

  private:
    ImageSize const _maxSize;
    std::shared_ptr<SixelColorPalette> _colors;
    ImageSize _size;
    Buffer _buffer; /// RGBA buffer
    /// Pixels per buffer row. The single authority for "where does row y start?".
    /// Invariant: _buffer.size() == _stride * _allocatedHeight * 4.
    unsigned _stride = 0;
    /// Rows currently backed by _buffer. The single authority for what storage exists.
    unsigned _allocatedHeight = 0;
    /// The background last passed to clear(); newly exposed storage is filled with it.
    RGBAColor _fillColor {};
    CellLocation _sixelCursor {};
    unsigned _currentColor = 0;
    /// _colors->at(_currentColor), memoized. Stable while the palette only grows.
    RGBColor _currentColorValue {};
    bool _explicitSize = false;
    /// Guards against finalize() running twice: SixelParser::done() calls it unconditionally, and
    /// re-compacting an already-compacted buffer would read past its end.
    bool _finalized = false;
    // This is an int because vt3xx takes the given ratio pan/pad and rounds up the ratio
    // to nearest integers. So 1:3 = 0.33 and it  becomes 1;
    unsigned int _aspectRatio;
    // Height of sixel band in pixels
    unsigned int _sixelBandHeight;
};

} // namespace vtbackend
