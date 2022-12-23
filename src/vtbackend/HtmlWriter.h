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

#include <vtbackend/Color.h>
#include <vtbackend/Line.h>
#include <vtbackend/primitives.h>

#include <unicode/convert.h>

#include <fmt/format.h>

#include <functional>
#include <ostream>
#include <sstream>
#include <vector>

namespace terminal
{

// Cell has the following:
// GraphicsRendition
// CellFlags bold, italic, etc
// underlineColor from CellExtra
// backgroundColor
// foregroundColor no css property

struct CssSelectorProperties
{
    uint8_t textAlign; // Options center, left, right, justify
    
    // uint8_t textDecoration; // All 3 below ones combined
    uint8_t textDecorationColor; // Option blue, none
    uint8_t textDecorationLine; // Options overline
    uint8_t textDecorationStyle; // Options dashed

    uint8_t textIndent; // Option 20px
    uint8_t textTransform; // Options capitalize, lowercase, uppercase

    uint8_t color; // Option RGBA, HSLA, RGB, HSL
    uint8_t backgroundColor; // Option RGBA, HSLA, RGB, HSL

    uint8_t fontFamily; // Options "Arial", "Times New Roman"
    uint8_t fontSize; // Options 22px
    uint8_t fontStyle; // Options italic, normal, oblique
    uint8_t fontWeight; // Options 100/ bold
    uint8_t wordSpacing;
    uint8_t lineHeight;

};

// Serializes text and SGR attributes into a valid VT stream.
class HtmlWriter
{
  public:
    using Writer = std::function<void(char const*, size_t)>;

    static constexpr inline auto MaxParameterCount = 16;

    explicit HtmlWriter(Writer writer);
    explicit HtmlWriter(std::ostream& output);
    explicit HtmlWriter(std::vector<char>& output);

    // Writes the given Line<> to the output stream without the trailing newline.
    template <typename Cell>
    void write(Line<Cell> const& line);

    /*template <typename... T>
    void write(fmt::format_string<T...> fmt, T&&... args);
    void write(std::string_view s);
    void write(char32_t v);*/
    void setCssRGBAColor(Color cellFlags);
    [[nodiscard]] std::string setCssTextFormatting(CellFlags cellFlags);

  private:
    Writer _writer;
    /*std::vector<unsigned> _sgr;
    std::stringstream sstr;
    std::vector<unsigned> _lastSGR;*/

    RGBAColor _currentCssRGBAColor; // background-color:rgba(red, green, blue, alpha)


    
};

} // namespace terminal
