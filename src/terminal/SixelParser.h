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
#include <terminal/ParserExtension.h>
#include <terminal/primitives.h>

#include <crispy/range.h>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace terminal
{

/// Sixel Stream Parser API.
///
/// Parses a sixel stream without any Sixel introducer CSI or ST to leave sixel mode,
/// that must be done by the parent parser.
///
/// TODO: make this parser O(1) with state table lookup tables, just like the VT parser.
class SixelParser: public ParserExtension
{
  public:
    enum class State
    {
        Ground,           // Sixel data
        RasterSettings,   // '"', configuring the raster
        RepeatIntroducer, // '!'
        ColorIntroducer,  // '#', color-set or color-use
        ColorParam        // color parameter
    };

    enum class Colorspace
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
        virtual void setColor(unsigned _index, RGBColor const& _color) = 0;

        /// Uses the given color for future paints
        virtual void useColor(unsigned _index) = 0;

        /// moves sixel-cursor to the left border
        virtual void rewind() = 0;

        /// moves the sixel-cursorto the left border of the next sixel-band
        virtual void newline() = 0;

        /// Defines the aspect ratio (pan / pad = aspect ratio) and image dimensions in pixels for
        /// the upcoming pixel data.
        virtual void setRaster(int _pan, int _pad, ImageSize _imageSize) = 0;

        /// renders a given sixel at the current sixel-cursor position.
        virtual void render(int8_t _sixel) = 0;

        /// Finalizes the image by optimizing the underlying storage to its minimal dimension in storage.
        virtual void finalize() = 0;
    };

    using OnFinalize = std::function<void()>;
    explicit SixelParser(Events& _events, OnFinalize _finisher = {});

    using iterator = char const*;

    void parseFragment(iterator _begin, iterator _end)
    {
        for (auto const ch: crispy::range(_begin, _end))
            parse(ch);
    }

    void parseFragment(std::string_view _range)
    {
        parseFragment(_range.data(), _range.data() + _range.size());
    }

    void parse(char _value);
    void done();

    static void parse(std::string_view _range, Events& _events)
    {
        auto parser = SixelParser { _events };
        parser.parseFragment(_range.data(), _range.data() + _range.size());
        parser.done();
    }

    // ParserExtension overrides
    void pass(char _char) override;
    void finalize() override;

  private:
    void paramShiftAndAddDigit(unsigned _value);
    void transitionTo(State _newState);
    void enterState();
    void leaveState();
    void fallback(char _value);

  private:
    State state_ = State::Ground;
    std::vector<unsigned> params_;

    Events& events_;
    OnFinalize finalizer_;
};

class SixelColorPalette
{
  public:
    SixelColorPalette(unsigned int _size, unsigned int _maxSize);

    void reset();

    [[nodiscard]] unsigned int size() const noexcept { return static_cast<unsigned int>(palette_.size()); }
    void setSize(unsigned int _newSize);

    [[nodiscard]] unsigned int maxSize() const noexcept { return maxSize_; }
    void setMaxSize(unsigned int _newSize);

    void setColor(unsigned int _index, RGBColor const& _color);
    [[nodiscard]] RGBColor at(unsigned int _index) const noexcept;

  private:
    std::vector<RGBColor> palette_;
    unsigned int maxSize_;
};

/// Sixel Image Builder API
///
/// Implements the SixelParser::Events event listener to construct a Sixel image.
class SixelImageBuilder: public SixelParser::Events
{
  public:
    using Buffer = std::vector<uint8_t>;

    SixelImageBuilder(ImageSize _maxSize,
                      int _aspectVertical,
                      int _aspectHorizontal,
                      RGBAColor _backgroundColor,
                      std::shared_ptr<SixelColorPalette> _colorPalette);

    [[nodiscard]] ImageSize maxSize() const noexcept { return maxSize_; }
    [[nodiscard]] ImageSize size() const noexcept { return size_; }
    [[nodiscard]] int aspectRatioNominator() const noexcept { return aspectRatio_.nominator; }
    [[nodiscard]] int aspectRatioDenominator() const noexcept { return aspectRatio_.denominator; }
    [[nodiscard]] RGBColor currentColor() const noexcept { return colors_->at(currentColor_); }

    [[nodiscard]] RGBAColor at(CellLocation _coord) const noexcept;

    [[nodiscard]] Buffer const& data() const noexcept { return buffer_; }
    [[nodiscard]] Buffer& data() noexcept { return buffer_; }

    void clear(RGBAColor _fillColor);

    void setColor(unsigned _index, RGBColor const& _color) override;
    void useColor(unsigned _index) override;
    void rewind() override;
    void newline() override;
    void setRaster(int _pan, int _pad, ImageSize _imageSize) override;
    void render(int8_t _sixel) override;
    void finalize() override;

    [[nodiscard]] CellLocation const& sixelCursor() const noexcept { return sixelCursor_; }

  private:
    void write(CellLocation const& _coord, RGBColor const& _value) noexcept;

  private:
    ImageSize const maxSize_;
    std::shared_ptr<SixelColorPalette> colors_;
    ImageSize size_;
    Buffer buffer_; /// RGBA buffer
    CellLocation sixelCursor_;
    unsigned currentColor_;
    struct
    {
        int nominator;
        int denominator;
    } aspectRatio_;
    bool explicitSize_ = false;
};

} // namespace terminal
