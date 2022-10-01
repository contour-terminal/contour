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
#include <terminal/primitives.h>

#if !defined(_WIN32)
    #include <terminal/pty/UnixUtils.h>
#endif

#include <text_shaper/fontconfig_locator.h>
#include <text_shaper/open_shaper.h>

#include <unicode/convert.h>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#if !defined(_WIN32)
    #include <sys/ioctl.h>
    #include <sys/select.h>

    #include <fcntl.h>
    #include <termios.h>
    #include <unistd.h>
#endif

using namespace std;
using namespace std::string_view_literals;

using terminal::CellLocation;
using terminal::ColumnCount;
using terminal::ColumnOffset;
using terminal::ImageSize;
using terminal::LineCount;
using terminal::LineOffset;
using terminal::PageSize;
using terminal::PixelCoordinate;

[[noreturn]] void fatal(string_view message)
{
    cerr << "Fatal: " << message;
    exit(EXIT_FAILURE);
}

struct AltScreen
{
    AltScreen() { fmt::print("\033[?1047h"); }

    ~AltScreen()
    {
        fmt::print("\033[?1047l");
        fmt::print("It's about time. Bye.\r\n");
    }
};

optional<PageSize> getPageSize()
{
    auto ws = winsize {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
        return nullopt;
    return PageSize { LineCount::cast_from(ws.ws_row), ColumnCount::cast_from(ws.ws_col) };
}

/// View over a grayscale image of given dimension and pixel data.
/// To be used to conveniently access pixels at given coordinates,
/// but also to be able to easily translate a pixel into more complex Unicode characters,
/// respecting neighbouring pixels.
struct GrayscaleCanvasView
{
    ImageSize size;
    gsl::span<uint8_t> pixels;

    // NB: Overshooting coordinates hould result into minimal intensity (0).
    [[nodiscard]] uint8_t at(PixelCoordinate coordiante) const noexcept;

    [[nodiscard]] char32_t sextant(PixelCoordinate coordinate) const noexcept;
};

/// Renderes the clock face with full unicode block character and different (RGB) grayscale
/// coloring to denote intensity of antialiased glyphs.
struct ClockRenderer
{
    terminal::PageSize pageSize {};
    CellLocation currentCursorPosition { LineOffset(-1), ColumnOffset(-1) };
    text::font_metrics metrics {};
    bool rendering = false;

    ClockRenderer(PageSize pageSize, text::font_metrics metrics):
        pageSize { pageSize }, metrics { std::move(metrics) }
    {
        fmt::print("\033[?25l"); // hide cursor
    }

    ~ClockRenderer()
    {
        fmt::print("\033[?25h"); // show cursor
        if (rendering)
            end();
    }

    void begin()
    {
        rendering = true;
        fmt::print("\033[?2026h"      // Synchronized Output: begin
                   "\033[3;1H"        // CUP (2, 1)
                   "\033[48;2;0;0;0m" // set background color to pure black
                   "\033[2J"          // ED (from cursor to page bottom)
        );
    }

    void end()
    {
        rendering = false;
        fmt::print("\033[?2026l" // Synchronized Output: flush
                   "\033[m"      // SGR reset
                   "\r\n"        // force flush
        );
    }

    void moveCursorTo(CellLocation cellLocation)
    {
        if (currentCursorPosition != cellLocation)
        {
            fmt::print("\033[{};{}H", cellLocation.line + 1, cellLocation.column + 1);
            currentCursorPosition = cellLocation;
        }
    }

    void renderGlyph(text::rasterized_glyph const& glyph,
                     text::glyph_position const& glyphPosition,
                     CellLocation baseLocation)
    {
        for (auto row = size_t { 0 }; row < unbox<size_t>(glyph.bitmapSize.height); ++row)
        {
            for (auto col = size_t { 0 }; col < unbox<size_t>(glyph.bitmapSize.width); ++col)
            {
                auto const value = glyph.bitmap.at(row * unbox<size_t>(glyph.bitmapSize.width) + col);
                // clang-format off
                auto const screenLine =
                                baseLocation.line
                                - LineOffset::cast_from(glyph.position.y)
                                + LineOffset::cast_from(glyphPosition.offset.y)
                                + LineOffset::cast_from(row);
                auto const screenColumn = baseLocation.column
                                + ColumnOffset::cast_from(glyph.position.x)
                                + ColumnOffset::cast_from(glyphPosition.offset.x)
                                + ColumnOffset::cast_from(col);
                // clang-format on

                renderPixelAt(CellLocation { screenLine, screenColumn }, value);
            }
        }
    }

    void renderPixelAt(CellLocation cellLocation, uint8_t intensity) // 0..255
    {
        moveCursorTo(cellLocation);

        if (intensity)
        {
            setTextColorGrayscale(intensity);
            fmt::print("█");
        }
        else
            fmt::print(" ");

        ++currentCursorPosition.column; // increment column, assume no line wrapping
    }

    void setTextColorGrayscale(uint8_t intensity)
    {
        if (currentTextColorGrayscale != static_cast<int>(intensity))
        {
            currentTextColorGrayscale = static_cast<int>(intensity);
            // fmt::print("\033[38:2:{v}:{v}:{v}m", fmt::arg("v", currentTextColorGrayscale));
            fmt::print("\033[38;2;{};{};{}m",
                       currentTextColorGrayscale,
                       currentTextColorGrayscale,
                       0 /*currentTextColorGrayscale*/);
        }
    }

    int currentTextColorGrayscale = -1;
};

struct NonBlocking
{
#if !defined(_WIN32)
    int const savedStdinFlags = fcntl(STDIN_FILENO, F_GETFL);
    int fd;

    explicit NonBlocking(int fd): fd { fd } { fcntl(fd, F_SETFL, savedStdinFlags | O_NONBLOCK); }

    ~NonBlocking() { fcntl(fd, F_SETFL, savedStdinFlags); }
#else
    template <typename T>
    explicit NonBlocking(T)
    {
    }
#endif
};

struct RawMode
{
#if !defined(_WIN32)
    termios const savedTerminalSettings = terminal::detail::getTerminalSettings(STDOUT_FILENO);
#endif

    RawMode()
    {
#if !defined(_WIN32)
        auto rawMode = termios {};
        rawMode.c_lflag = (rawMode.c_lflag & tcflag_t(~(ECHO | ICANON)));
        terminal::detail::applyTerminalSettings(STDOUT_FILENO, rawMode);
#endif
    }

    ~RawMode()
    {
#if !defined(_WIN32)
        terminal::detail::applyTerminalSettings(STDOUT_FILENO, savedTerminalSettings);
#endif
    }

    [[nodiscard]] bool shouldQuit() noexcept
    {
#if !defined(_WIN32)
        auto const _ = NonBlocking(STDIN_FILENO);

        char buf[128];
        auto const rv = ::read(STDIN_FILENO, &buf, sizeof(buf));
        return rv > 0 || (rv < 0 && (errno == EAGAIN || errno == EBUSY));
#endif
    }
};

template <typename T>
void renderClock(text::open_shaper& textShaper, text::font_key fontKey, std::tm const& tm, T renderOne)
{
    auto const textUtf8 = fmt::format("{:%T}", tm);
    auto const text = unicode::convert_to<char32_t>(string_view(textUtf8.data(), textUtf8.size()));

    auto textClusters = vector<unsigned> {};
    generate_n(back_inserter(textClusters), text.size(), [n = size_t { 1 }]() mutable { return n++; });
    auto const textClustersSpan = gsl::span(textClusters.data(), textClusters.size());

    auto glyphPositions = text::shape_result {};

    textShaper.shape(fontKey,
                     text,
                     textClustersSpan,
                     unicode::Script::Latin,
                     unicode::PresentationStyle::Text,
                     glyphPositions);

    for (auto const& glyphPosition: glyphPositions)
        renderOne(glyphPosition);
}

ColumnCount estimateTextWidth(text::open_shaper& textShaper, text::font_key fontKey)
{
    auto usedColumns = ColumnCount(0);
    renderClock(textShaper, fontKey, std::tm {}, [&](text::glyph_position const& glyphPosition) {
        usedColumns += ColumnCount::cast_from(glyphPosition.advance.x);
    });
    return usedColumns;
}

/// Possible Usage Proposal:
///
///   chrono [config PATH] [family FAMILY=monospace] [bold] [italic] [size PT=auto] [color COLOR] [background
///   COLOR] (clock | stopwatch | timer [[HH:]MM:]SS)
///
///  Options:
///    config PATH       load defaults from given config file (default ${XDG_CONFIG_HOME}/contour/chrono.yml)
///    family NAME       font family name
///    bold              font weight is bold
///    italic            font slant is italic
///    size POINTS       font size is given POINTS size (default: auto)
///    color             RGB color for text, in standard #RRGGBB syntax or "transparent"
///    background        RGB color for background, in standard #RRGGBB syntax or "transparent" (default:
///    transparent)
///
///  Actions:
///    clock        Shows a clock face with the current time in HH:MM:SS format
///    timer        Shows a timer, counting down, in MM:SS format
///    stopwatch    Shows a stopwatch counting up in MM:SS.NNN format
///
///  Example Uses:
///
///     contour tool chrono family "Times New Roman" timer 05:00
///     contour tool chrono family "JetBrainsMono Nerd Font Mono" bold italic clock
///     contour tool chrono color "#FF6600" stopwatch
///
/// The Config file contains simple key value pairs (passed like arguments) to customize
/// preferred defaults. The file format will be YAML or DOS-INI-alike.
///
int main(int, char const*[])
{
    auto const altScreen = AltScreen();
    auto rawMode = RawMode();

    auto fontLocator = text::fontconfig_locator();
    auto textShaper = make_unique<text::open_shaper>(text::DPI { 96, 96 }, fontLocator);

    // TODO: configurable
    auto fontDescription = text::font_description {};
    fontDescription.familyName = "JetBrainsMono Nerd Font Mono";
    // fontDescription.familyName = "monospace";
    fontDescription.weight = text::font_weight::bold; // TODO(pr) configurable
    fontDescription.slant = text::font_slant::normal; // TODO(pr) configurable

    auto const pageSize = getPageSize().value();
    auto const fontSize = text::font_size { unbox<double>(pageSize.lines) * 0.5 };
    auto const fontKey = textShaper->load_font(fontDescription, fontSize).value();
    auto const fontMetrics = textShaper->metrics(fontKey);
    auto const fontBaseline = fontMetrics.line_height - fontMetrics.ascender;
    auto const screenBaseLine = LineOffset::cast_from(pageSize.lines)
                                - (unbox<int>(pageSize.lines) - fontMetrics.line_height) / 2
                                - LineOffset::cast_from(fontBaseline);
    auto const screenBaseColumn =
        ColumnOffset::cast_from(pageSize.columns - estimateTextWidth(*textShaper, fontKey)) / 2;

    auto renderer = make_unique<ClockRenderer>(pageSize, fontMetrics);

    while (!rawMode.shouldQuit())
    {
        auto const nowSeconds = chrono::system_clock::to_time_t(chrono::system_clock::now());

        renderer->begin();
        auto penCellPosition = CellLocation { screenBaseLine, screenBaseColumn };
        renderClock(*textShaper, fontKey, *localtime(&nowSeconds), [&](text::glyph_position const& gpos) {
            auto const rasterizedGlyph = textShaper->rasterize(gpos.glyph, text::render_mode::gray);
            if (rasterizedGlyph.has_value())
                renderer->renderGlyph(rasterizedGlyph.value(), gpos, penCellPosition);
            penCellPosition.column += ColumnOffset(gpos.advance.x);
            penCellPosition.line += LineOffset(gpos.advance.y);
        });
        renderer->end();

        // TODO: instead of sleep we should wait for keyboard input for up to 1 second
        this_thread::sleep_for(chrono::seconds(1));
    }

    return EXIT_SUCCESS;
}
