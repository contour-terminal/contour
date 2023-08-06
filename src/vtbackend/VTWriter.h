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

#include <fmt/format.h>

#include <functional>
#include <ostream>
#include <sstream>
#include <vector>

#include <libunicode/convert.h>

namespace terminal
{

// Serializes text and SGR attributes into a valid VT stream.
class VTWriter
{
  public:
    using Writer = std::function<void(char const*, size_t)>;

    static constexpr inline auto maxParameterCount = 16;

    explicit VTWriter(Writer writer);
    explicit VTWriter(std::ostream& output);
    explicit VTWriter(std::vector<char>& output);

    void crlf();

    // Writes the given Line<> to the output stream without the trailing newline.
    template <typename Cell>
    void write(Line<Cell> const& line);

    template <typename... T>
    void write(fmt::format_string<T...> fmt, T&&... args);
    void write(std::string_view s);
    void write(char32_t v);

    void sgrFlush();
    void sgrAdd(unsigned n);
    void sgrRewind();
    void sgrAdd(graphics_rendition m);
    void setForegroundColor(color color);
    void setBackgroundColor(color color);

    void sgrAddExplicit(unsigned n);

    template <typename... Args>
    void sgrAdd(unsigned n, Args... values)
    {
        if (_sgr.size() + sizeof...(values) > maxParameterCount)
            sgrFlush();

        sgrAddExplicit(n);
        (sgrAddExplicit(static_cast<unsigned>(values)), ...);
    }

  private:
    static std::string sgrFlush(std::vector<unsigned> const& sgr);

    Writer _writer;
    std::vector<unsigned> _sgr;
    std::stringstream _sstr;
    std::vector<unsigned> _lastSGR;
    color _currentForegroundColor = DefaultColor();
    color _currentUnderlineColor = DefaultColor();
    color _currentBackgroundColor = DefaultColor();
};

template <typename... T>
inline void VTWriter::write(fmt::format_string<T...> fmt, T&&... args)
{
    write(fmt::vformat(fmt, fmt::make_format_args(args...)));
}

inline void VTWriter::crlf()
{
    write("\r\n");
}

} // namespace terminal
