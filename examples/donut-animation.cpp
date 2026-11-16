// SPDX-License-Identifier: Apache-2.0

/// @file donut-animation.cpp
/// A spinning, lit, truecolour torus drawn in text across the whole terminal, and an example of how a
/// full-screen animation should talk to a terminal so that it stays fast at any size -- the 400x120 cells
/// of a tiny font included:
///
/// - **One write per frame.** The frame is encoded into one string and written with one call. Writing cell
///   by cell, or flushing a stream per cell, costs a system call per cell and wakes the terminal's reader
///   for every few bytes, which then pays its whole per-read cost each time.
/// - **Synchronized output** (DEC mode 2026) around each frame, so the terminal shows whole frames instead
///   of whatever part of one had arrived when it repainted: no tearing.
/// - **Each row is addressed** with CUP rather than ended with a newline: a newline after the last row
///   scrolls the whole screen.
/// - **Colour is sent only where it changes**, and never for a blank, which looks the same in every
///   foreground colour. Colours are quantized to the shading's steps, so neighbouring cells share them.
/// - **The work scales with the screen**: the surface is sampled a fixed number of times per cell, and
///   frames are paced to a frame rate rather than to a fixed sleep after each one.
/// - **The terminal is always restored**: every way the program ends -- a signal, an exception -- goes
///   through one destructor on the main thread.
///
/// Usage: donut-animation [FRAMES-PER-SECOND]
///
/// The frame rate defaults to 240; match it to the display. A terminal application cannot ask what the
/// display's refresh rate is, and a terminal cannot show frames faster than it repaints, so frames beyond
/// that rate are bytes it parses only to overwrite them unseen -- while a rate below it leaves a faster
/// display's refreshes with no new frame.
///
/// Press Ctrl+C to stop. Ctrl+Z and Ctrl+\ are disabled while it runs: both would leave the shell on the
/// alternate screen with the cursor hidden.
///
/// Grown from a program a user shared on Contour's Discord to report slow, tearing animation, which wrote
/// each cell with its own flushed write, synchronized nothing, and scrolled the screen once per frame.

#include <sys/ioctl.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <iterator>
#include <numbers>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <termios.h>
#include <unistd.h>

#include "WriteToTTY.hpp"

using namespace std::string_view_literals;

namespace
{

/// The frame rate when none is given: enough for a 240 Hz display, and well within reach -- without a cap,
/// the animation draws over 500 frames per second even at 400x120.
constexpr auto DefaultFramesPerSecond = 240;

/// The highest frame rate accepted, beyond any display's refresh rate.
constexpr auto MaxFramesPerSecond = 1000;

/// The longest step, in seconds, the animation advances by in one frame, so that a stall does not make the
/// torus jump.
constexpr auto MaxTimeStep = 0.1;

/// Surface samples around the torus' outer circle per cell of the screen's longer side; the tube's circle
/// gets fewer, in proportion to its smaller radius. The torus' image is scaled to the screen, so a circle
/// spans a number of cells proportional to the screen's size, and sampling it more densely than this only
/// redraws cells already drawn. Measured against 40 samples per cell over 300 orientations, 2 lights
/// 99.1-99.8 % of the cells at 80x24 to 400x120, missing only single cells on the silhouette; 1 leaves
/// 0.8-1.7 % of them as holes.
constexpr auto SamplesPerCell = 2.0;

/// The fewest samples taken around either circle, so that a tiny screen still draws a closed ring.
constexpr auto MinimumSamples = 64.0;

/// The glyphs a surface point is drawn with, from facing away from the light to facing it squarely. Each
/// is one step of the shading, and the colour takes the same steps.
constexpr auto ShadeGlyphs = "FEDCBA9876543210$%@#*"sv;

/// The horizontal and vertical size of the torus' image, as a fraction of the screen's width and height.
constexpr auto ImageScale = 0.35;

/// Set from the signal handler to end the animation. The terminal is restored by the main loop rather than by
/// the handler: a signal can arrive in the middle of a frame's write, and restoring bytes written from the
/// handler would land inside a half-written escape sequence or an unfinished synchronized update.
std::atomic<bool> stopRequested { false };

/// Asks the animation to stop. The parameter is the signal number, which std::signal's handler type
/// requires.
void onSignal(int /*signal*/) noexcept
{
    stopRequested = true;
}

/// A point or a direction in the torus' three-dimensional space.
struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    [[nodiscard]] constexpr Vec3 operator*(double factor) const noexcept
    {
        return { .x = x * factor, .y = y * factor, .z = z * factor };
    }

    [[nodiscard]] constexpr double dot(Vec3 const& other) const noexcept
    {
        return (x * other.x) + (y * other.y) + (z * other.z);
    }

    /// @return This direction scaled to length 1, or the zero vector when it has no direction.
    [[nodiscard]] Vec3 normalized() const noexcept
    {
        auto const length = std::sqrt(dot(*this));
        return length > 0.0 ? *this * (1.0 / length) : Vec3 {};
    }
};

/// An angle, kept as the cosine and sine every use of it needs.
struct UnitAngle
{
    double cos = 1.0;
    double sin = 0.0;
};

/// @return @p count angles evenly spaced around a full circle, rounded up to a whole number.
std::vector<UnitAngle> sampleCircle(double count)
{
    auto const samples = static_cast<int>(std::ceil(count));
    auto const step = 2.0 * std::numbers::pi / samples;
    auto angles = std::vector<UnitAngle> {};
    angles.reserve(static_cast<size_t>(samples));
    for (auto const index: std::views::iota(0, samples))
    {
        auto const angle = index * step;
        angles.push_back({ .cos = std::cos(angle), .sin = std::sin(angle) });
    }
    return angles;
}

/// The torus' orientation, turned about the x, y and z axes in that order, and how fast it turns.
struct Rotation
{
    Vec3 angles {};     ///< The angle about each axis, in radians.
    Vec3 velocities {}; ///< The angular velocity about each axis, in radians per second.

    /// Turns the torus on by @p seconds' worth of its velocities.
    void advance(double seconds) noexcept
    {
        angles = { .x = angles.x + (velocities.x * seconds),
                   .y = angles.y + (velocities.y * seconds),
                   .z = angles.z + (velocities.z * seconds) };
    }
};

/// A Rotation's current angles, with their sines and cosines taken once per frame rather than once per
/// point.
class Rotator
{
  public:
    explicit Rotator(Vec3 const& angles) noexcept:
        _x { .cos = std::cos(angles.x), .sin = std::sin(angles.x) },
        _y { .cos = std::cos(angles.y), .sin = std::sin(angles.y) },
        _z { .cos = std::cos(angles.z), .sin = std::sin(angles.z) }
    {
    }

    /// @return @p v turned about the x, then the y, then the z axis.
    [[nodiscard]] Vec3 apply(Vec3 const& v) const noexcept
    {
        auto const aboutX =
            Vec3 { .x = v.x, .y = (v.y * _x.cos) - (v.z * _x.sin), .z = (v.y * _x.sin) + (v.z * _x.cos) };
        auto const aboutY = Vec3 { .x = (aboutX.x * _y.cos) + (aboutX.z * _y.sin),
                                   .y = aboutX.y,
                                   .z = (aboutX.z * _y.cos) - (aboutX.x * _y.sin) };
        return { .x = (aboutY.x * _z.cos) - (aboutY.y * _z.sin),
                 .y = (aboutY.x * _z.sin) + (aboutY.y * _z.cos),
                 .z = aboutY.z };
    }

  private:
    UnitAngle _x;
    UnitAngle _y;
    UnitAngle _z;
};

/// A torus around the z axis: a tube of minorRadius swept around a circle of majorRadius.
struct Torus
{
    double majorRadius = 2.0;
    double minorRadius = 1.0;

    /// @param theta The angle around the tube.
    /// @param phi The angle around the z axis.
    /// @return The surface point at (@p theta, @p phi).
    [[nodiscard]] constexpr Vec3 pointAt(UnitAngle theta, UnitAngle phi) const noexcept
    {
        auto const ringRadius = majorRadius + (minorRadius * theta.cos);
        return { .x = ringRadius * phi.cos, .y = ringRadius * phi.sin, .z = minorRadius * theta.sin };
    }

    /// @return The outward unit normal at the surface point (@p theta, @p phi).
    [[nodiscard]] static constexpr Vec3 normalAt(UnitAngle theta, UnitAngle phi) noexcept
    {
        return { .x = theta.cos * phi.cos, .y = theta.cos * phi.sin, .z = theta.sin };
    }
};

/// A terminal's size in character cells.
struct ScreenSize
{
    int columns = 80;
    int lines = 24;

    constexpr bool operator==(ScreenSize const&) const noexcept = default;
};

/// A 24-bit colour.
struct Rgb
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    constexpr bool operator==(Rgb const&) const noexcept = default;
};

/// What one cell shows, and how near to the viewer the surface it shows is.
struct Cell
{
    char glyph = ' ';
    Rgb color {};
    double inverseDepth = 0.0; ///< 1 / distance from the viewer; 0 for a cell nothing is drawn in.
};

/// One frame's cells, drawn with a depth test so that the surface nearest the viewer wins.
class FrameBuffer
{
  public:
    explicit FrameBuffer(ScreenSize size):
        _size { size }, _cells(static_cast<size_t>(size.columns) * static_cast<size_t>(size.lines))
    {
    }

    [[nodiscard]] ScreenSize size() const noexcept { return _size; }

    /// Empties every cell.
    void clear() noexcept { std::ranges::fill(_cells, Cell {}); }

    /// Draws @p cell at (@p column, @p line) unless it lies off the screen or behind what is already drawn
    /// there.
    void plot(int column, int line, Cell const& cell) noexcept
    {
        if (column < 0 || column >= _size.columns || line < 0 || line >= _size.lines)
            return;
        auto& target = _cells[index(column, line)];
        if (cell.inverseDepth > target.inverseDepth)
            target = cell;
    }

    /// @return The cell at (@p column, @p line), which must lie on the screen.
    [[nodiscard]] Cell const& at(int column, int line) const noexcept { return _cells[index(column, line)]; }

  private:
    [[nodiscard]] size_t index(int column, int line) const noexcept
    {
        return (static_cast<size_t>(line) * static_cast<size_t>(_size.columns)) + static_cast<size_t>(column);
    }

    ScreenSize _size;
    std::vector<Cell> _cells;
};

/// Where a point lands on the screen, and how near to the viewer it is.
struct Projection
{
    int column = 0;
    int line = 0;
    double inverseDepth = 0.0;
};

/// A perspective camera on the negative z axis, looking at the origin, with an image scaled to the screen
/// it draws on.
class Camera
{
  public:
    explicit Camera(ScreenSize screen) noexcept:
        _screen { screen }, _scaleX { screen.columns * ImageScale }, _scaleY { screen.lines * ImageScale }
    {
    }

    /// @return Where @p point lands on the screen, or nothing when it lies at or behind the near plane.
    [[nodiscard]] std::optional<Projection> project(Vec3 const& point) const noexcept
    {
        auto const depth = Distance + point.z;
        if (depth <= NearPlane)
            return std::nullopt;
        auto const inverseDepth = 1.0 / depth;
        return Projection {
            .column =
                static_cast<int>(std::floor((_screen.columns * 0.5) + (_scaleX * point.x * inverseDepth))),
            .line = static_cast<int>(std::floor((_screen.lines * 0.5) - (_scaleY * point.y * inverseDepth))),
            .inverseDepth = inverseDepth,
        };
    }

  private:
    static constexpr auto Distance = 5.0;  ///< From the camera to the torus' centre.
    static constexpr auto NearPlane = 0.1; ///< Nearer than this, nothing is drawn.

    ScreenSize _screen;
    double _scaleX;
    double _scaleY;
};

/// @return The cell a surface point is drawn as: its glyph and colour from how squarely it faces the light.
Cell shade(Vec3 const& normal, Vec3 const& light, double inverseDepth) noexcept
{
    constexpr auto Steps = ShadeGlyphs.size() - 1;
    auto const brightness = std::clamp(normal.dot(light), 0.0, 1.0);
    auto const step = static_cast<size_t>(brightness * static_cast<double>(Steps));
    // Quantized to the glyph's step rather than taken from the brightness itself: 256 levels would give
    // nearly every lit cell a colour of its own, and so an escape sequence of its own.
    auto const intensity = static_cast<uint8_t>(step * 255 / Steps);
    return Cell {
        .glyph = ShadeGlyphs[step],
        .color = { .red = intensity,
                   .green = static_cast<uint8_t>(intensity / 2),
                   .blue = static_cast<uint8_t>(255 - intensity) },
        .inverseDepth = inverseDepth,
    };
}

/// Draws @p torus, turned to @p angles and lit from the direction @p light, into @p frame.
void drawTorus(FrameBuffer& frame, Torus const& torus, Vec3 const& angles, Vec3 const& light)
{
    auto const screen = frame.size();
    auto const camera = Camera { screen };
    auto const rotator = Rotator { angles };

    auto const longerSide = static_cast<double>(std::max(screen.columns, screen.lines));
    auto const outerSamples = std::max(MinimumSamples, SamplesPerCell * longerSide);
    auto const tubeSamples =
        std::max(MinimumSamples, outerSamples * torus.minorRadius / (torus.majorRadius + torus.minorRadius));
    auto const thetas = sampleCircle(tubeSamples);
    auto const phis = sampleCircle(outerSamples);

    for (auto const& theta: thetas)
    {
        for (auto const& phi: phis)
        {
            auto const projection = camera.project(rotator.apply(torus.pointAt(theta, phi)));
            if (!projection)
                continue;
            auto const normal = rotator.apply(Torus::normalAt(theta, phi));
            frame.plot(projection->column, projection->line, shade(normal, light, projection->inverseDepth));
        }
    }
}

/// Encodes @p frame into @p out, replacing what it held, as the bytes that draw it.
/// @param frame The frame to encode.
/// @param out Where the bytes go; passed in so that its capacity is reused from frame to frame.
void encodeFrame(FrameBuffer const& frame, std::string& out)
{
    auto const size = frame.size();
    auto inserter = std::back_inserter(out);
    auto pen = std::optional<Rgb> {};

    out.clear();
    out +=
        "\033[?2026h"; // Begin synchronized update: the terminal shows nothing of this frame until it ends.
    for (auto const line: std::views::iota(0, size.lines))
    {
        std::format_to(inserter, "\033[{};1H", line + 1);
        for (auto const column: std::views::iota(0, size.columns))
        {
            auto const& cell = frame.at(column, line);
            if (cell.glyph != ' ' && pen != cell.color)
            {
                std::format_to(
                    inserter, "\033[38;2;{};{};{}m", cell.color.red, cell.color.green, cell.color.blue);
                pen = cell.color;
            }
            out += cell.glyph;
        }
    }
    out += "\033[?2026l"; // End synchronized update: the terminal shows the whole frame at once.
}

/// The terminal, switched to the alternate screen with the cursor hidden, autowrap off and input not echoed
/// for as long as this lives; restored as it was when it is destroyed.
class Terminal
{
  public:
    /// Prepares the terminal on standard output for the animation.
    /// @return The prepared terminal, or why standard output is not one.
    [[nodiscard]] static std::expected<Terminal, std::string> open()
    {
        auto saved = termios {};
        if (tcgetattr(STDOUT_FILENO, &saved) != 0)
            return std::unexpected(std::string { "standard output is not a terminal" });

        // No echo and no line editing, and no keys that suspend (Ctrl+Z) or core-dump (Ctrl+\) the program:
        // either would leave the shell on the alternate screen. Ctrl+C still interrupts.
        auto animating = saved;
        animating.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
        animating.c_cc[VSUSP] = _POSIX_VDISABLE;
        animating.c_cc[VQUIT] = _POSIX_VDISABLE;
        tcsetattr(STDOUT_FILENO, TCSAFLUSH, &animating);

        // Autowrap off: a frame places every row itself, and its last column must not move the cursor on.
        writeToTTY("\033[?1049h\033[?25l\033[?7l\033[2J");
        return Terminal { saved };
    }

    Terminal(Terminal&& other) noexcept: _saved { std::exchange(other._saved, std::nullopt) } {}
    Terminal& operator=(Terminal&&) = delete;
    Terminal(Terminal const&) = delete;
    Terminal& operator=(Terminal const&) = delete;

    ~Terminal()
    {
        if (!_saved)
            return;
        writeToTTY("\033[0m\033[?7h\033[?25h\033[?1049l");
        tcsetattr(STDOUT_FILENO, TCSAFLUSH, &*_saved);
    }

    /// @return The terminal's current size, or 80x24 when it does not say.
    [[nodiscard]] static ScreenSize size() noexcept
    {
        auto window = winsize {};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0 || window.ws_col == 0 || window.ws_row == 0)
            return ScreenSize {};
        return ScreenSize { .columns = window.ws_col, .lines = window.ws_row };
    }

  private:
    explicit Terminal(termios saved) noexcept: _saved { saved } {}

    std::optional<termios> _saved; ///< The mode to restore; empty once moved from.
};

/// @return The time one frame is given, from the frame rate in @p arguments (the program's arguments,
///         without its name), or why that is not one.
std::expected<std::chrono::steady_clock::duration, std::string> frameDuration(
    std::span<char const* const> arguments)
{
    auto framesPerSecond = DefaultFramesPerSecond;
    if (arguments.size() > 1)
        return std::unexpected(std::string { "usage: donut-animation [FRAMES-PER-SECOND]" });
    if (arguments.size() == 1)
    {
        auto const text = std::string_view { arguments.front() };
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), framesPerSecond);
        if (error != std::errc {} || end != text.data() + text.size() || framesPerSecond < 1
            || framesPerSecond > MaxFramesPerSecond)
            return std::unexpected(
                std::format("frames per second must be 1 to {}, not '{}'", MaxFramesPerSecond, text));
    }
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds { 1 })
           / framesPerSecond;
}

} // namespace

// A function-try-block: an exception -- an allocation failing for an absurd terminal size, say -- unwinds the
// Terminal and restores the screen before the message is printed, instead of terminating with neither.
int main(int argc, char const* argv[])
try
{
    auto const frameTime = frameDuration(std::span(argv, static_cast<size_t>(argc)).subspan(1));
    if (!frameTime)
    {
        std::println(stderr, "donut-animation: {}", frameTime.error());
        return EXIT_FAILURE;
    }

    // Installed before the terminal is switched, so that no signal finds it switched and nothing to restore
    // it.
    for (auto const signal: { SIGINT, SIGTERM, SIGHUP, SIGQUIT })
        std::signal(signal, onSignal);

    auto terminal = Terminal::open();
    if (!terminal)
    {
        std::println(stderr, "donut-animation: {}", terminal.error());
        return EXIT_FAILURE;
    }

    using Clock = std::chrono::steady_clock;

    auto const torus = Torus { .majorRadius = 2.0, .minorRadius = 1.0 };
    auto const light = Vec3 { .x = 2.4, .y = 2.6, .z = -2.1 }.normalized();
    auto rotation = Rotation { .angles = {}, .velocities = { .x = 1.2, .y = 1.8, .z = 1.6 } };
    auto frame = FrameBuffer { Terminal::size() };
    auto encoded = std::string {};
    auto previousFrame = Clock::now();

    while (!stopRequested)
    {
        auto const frameStart = Clock::now();
        rotation.advance(
            std::min(std::chrono::duration<double>(frameStart - previousFrame).count(), MaxTimeStep));
        previousFrame = frameStart;

        // Following the terminal's size rescales the torus to a resized window, a smaller font's included.
        if (auto const size = Terminal::size(); size != frame.size())
            frame = FrameBuffer { size };

        frame.clear();
        drawTorus(frame, torus, rotation.angles, light);
        encodeFrame(frame, encoded);
        writeToTTY(encoded);

        // Until the next frame is due, not for a fixed time after this one: drawing counts against the
        // budget.
        std::this_thread::sleep_until(frameStart + *frameTime);
    }
    return EXIT_SUCCESS;
}
catch (std::exception const& error)
{
    std::println(stderr, "donut-animation: {}", error.what());
    return EXIT_FAILURE;
}
