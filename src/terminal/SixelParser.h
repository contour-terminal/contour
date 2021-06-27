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

#include <terminal/Color.h>
#include <terminal/Coordinate.h>
#include <terminal/ParserExtension.h>

#include <crispy/range.h>
#include <crispy/size.h>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace terminal {

/// Sixel Stream Parser API.
///
/// Parses a sixel stream without any Sixel introducer CSI or ST to leave sixel mode,
/// that must be done by the parent parser.
///
/// TODO: make this parser O(1) with state table lookup tables, just like the VT parser.
class SixelParser : public ParserExtension
{
  public:
    enum class State {
        Ground,             // Sixel data
        RasterSettings,     // '"', configuring the raster
        RepeatIntroducer,   // '!'
        ColorIntroducer,    // '#', color-set or color-use
        ColorParam          // color parameter
    };

    enum class Colorspace { RGB, HSL };

    /// SixelParser's event handler
    class Events
    {
      public:
        virtual ~Events() = default;

        /// Defines a new color at given register index.
        virtual void setColor(unsigned _index, RGBColor const& _color) = 0;

        /// Uses the given color for future paints
        virtual void useColor(unsigned _index) = 0;

        /// moves sixel-cursor to the left border
        virtual void rewind() = 0;

        /// moves the sixel-cursorto the left border of the next sixel-band
        virtual void newline() = 0;

        /// Defines the aspect ratio (pan / pad = aspect ratio) and image dimensions in pixels for
        /// the upcoming pixel data.
        virtual void setRaster(unsigned _pan, unsigned _pad, crispy::Size const& _imageSize) = 0;

        /// renders a given sixel at the current sixel-cursor position.
        virtual void render(int8_t _sixel) = 0;
    };

    using OnFinalize = std::function<void()>;
    explicit SixelParser(Events& _events, OnFinalize _finisher = {});

    using iterator = char32_t const*;

    void parseFragment(iterator _begin, iterator _end)
    {
        for (auto const ch : crispy::range(_begin, _end))
            parse(ch);
    }

    void parseFragment(std::u32string_view _range)
    {
        parseFragment(_range.data(), _range.data() + _range.size());
    }

    /// This function is only used in unit tests.
    void parseFragment(std::string_view _range)
    {
        for (auto const ch: _range)
            parse(static_cast<char32_t>(ch));
    }

    void parse(char32_t _value);
    void done();

    static void parse(std::u32string_view _range, Events& _events)
    {
        auto parser = SixelParser{_events};
        parser.parseFragment(_range.data(), _range.data() + _range.size());
        parser.done();
    }

    // ParserExtension overrides
    void start() override;
    void pass(char32_t _char) override;
    void finalize() override;

  private:
    void paramShiftAndAddDigit(unsigned _value);
    void transitionTo(State _newState);
    void enterState();
    void leaveState();
    void fallback(char32_t _value);

  private:
    State state_ = State::Ground;
    std::vector<unsigned> params_;

    Events& events_;
    OnFinalize finalizer_;
};

class SixelColorPalette
{
  public:
    SixelColorPalette(size_t _size, size_t _maxSize);

    void reset();

    size_t size() const noexcept { return palette_.size(); }
    void setSize(size_t _newSize);

    size_t maxSize() const noexcept { return maxSize_; }
    void setMaxSize(size_t _newSize);

    void setColor(unsigned _index, RGBColor const& _color);
    RGBColor at(unsigned _index) const noexcept;

  private:
    std::vector<RGBColor> palette_;
    size_t maxSize_;
};

/// Sixel Image Builder API
///
/// Implements the SixelParser::Events event listener to construct a Sixel image.
class SixelImageBuilder : public SixelParser::Events
{
  public:
    using Buffer = std::vector<uint8_t>;

    SixelImageBuilder(crispy::Size const& _maxSize,
                      unsigned _aspectVertical,
                      unsigned _aspectHorizontal,
                      RGBAColor _backgroundColor,
                      std::shared_ptr<SixelColorPalette> _colorPalette);

    crispy::Size const& maxSize() const noexcept { return maxSize_; }

    crispy::Size const& size() const noexcept { return size_; }
    unsigned aspectRatioNominator() const noexcept { return aspectRatio_.nominator; }
    unsigned aspectRatioDenominator() const noexcept { return aspectRatio_.denominator; }
    RGBColor currentColor() const noexcept { return colors_->at(currentColor_); }

    RGBAColor at(Coordinate _coord) const noexcept;

    Buffer const& data() const noexcept { return buffer_; }
    Buffer& data() noexcept { return buffer_; }

    void clear(RGBAColor _fillColor);

    void setColor(unsigned _index, RGBColor const& _color) override;
    void useColor(unsigned _index) override;
    void rewind() override;
    void newline() override;
    void setRaster(unsigned _pan, unsigned _pad, crispy::Size const& _imageSize) override;
    void render(int8_t _sixel) override;

    Coordinate const& sixelCursor() const noexcept { return sixelCursor_; }

  private:
    void write(Coordinate const& _coord, RGBColor const& _value) noexcept;

  private:
    crispy::Size const maxSize_;
    std::shared_ptr<SixelColorPalette> colors_;
    crispy::Size size_;
    Buffer buffer_; /// RGBA buffer
    Coordinate sixelCursor_;
    unsigned currentColor_;
    struct {
        unsigned nominator;
        unsigned denominator;
    } aspectRatio_;
};

} // end namespace
