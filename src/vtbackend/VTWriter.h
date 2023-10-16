// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/Line.h>
#include <vtbackend/primitives.h>

#include <libunicode/convert.h>

#include <fmt/format.h>

#include <functional>
#include <ostream>
#include <sstream>
#include <vector>

namespace vtbackend
{

// Serializes text and SGR attributes into a valid VT stream.
class VTWriter
{
  public:
    using Writer = std::function<void(char const*, size_t)>;

    static constexpr inline auto MaxParameterCount = 16;

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
    void sgrAdd(GraphicsRendition m);
    void setForegroundColor(Color color);
    void setBackgroundColor(Color color);

    void sgrAddExplicit(unsigned n);

    template <typename... Args>
    void sgrAdd(unsigned n, Args... values)
    {
        if (_sgr.size() + sizeof...(values) > MaxParameterCount)
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
    Color _currentForegroundColor = DefaultColor();
    Color _currentUnderlineColor = DefaultColor();
    Color _currentBackgroundColor = DefaultColor();
};

template <typename... Ts>
inline void VTWriter::write(fmt::format_string<Ts...> fmt, Ts&&... args)
{
#if defined(__APPLE__)
    write(fmt::vformat(fmt, fmt::make_format_args(args...)));
#else
    write(fmt::vformat(fmt, fmt::make_format_args(std::forward<Ts>(args)...)));
#endif
}

inline void VTWriter::crlf()
{
    write("\r\n");
}

} // namespace vtbackend
