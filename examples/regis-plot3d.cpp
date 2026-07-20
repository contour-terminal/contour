// SPDX-License-Identifier: Apache-2.0

/// @file regis-plot3d.cpp
/// An interactive 3D plot of a complex function, rendered with Contour's ReGIS graphics and rotated
/// with the mouse. It plots the magnitude surface |f(z)| of f(z) = z^2 - 1 over a patch of the
/// complex plane, drawing a wireframe grid and the X/Y/Z axes. Drag with the left mouse button to
/// rotate (horizontal drag = yaw, vertical drag = pitch); press 'q' or Ctrl+C to quit.
///
/// The program enables SGR mouse reporting (DEC modes 1000/1002/1006), reads drag events, updates
/// the rotation, and re-emits the ReGIS scene each frame -- so it also demonstrates driving ReGIS
/// interactively from terminal input.

#include <array>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include <termios.h>
#include <unistd.h>

using namespace std::string_view_literals;

namespace
{

std::sig_atomic_t volatile running = 1;

void onSignal(int) noexcept
{
    running = 0;
}

void writeToTTY(std::string_view s) noexcept
{
    ::write(STDOUT_FILENO, s.data(), s.size());
}

void regis(std::string const& commands)
{
    writeToTTY("\033Pp");
    writeToTTY(commands);
    writeToTTY("\033\\");
}

// --- terminal raw-mode + mouse tracking RAII --------------------------------------------------

class RawMouseMode
{
  public:
    RawMouseMode()
    {
        tcgetattr(STDIN_FILENO, &_original);
        auto raw = _original;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        writeToTTY("\033[?1000;1002;1006h"); // button + drag reporting, SGR encoding
    }

    ~RawMouseMode()
    {
        writeToTTY("\033[?1000;1002;1006l");
        tcsetattr(STDIN_FILENO, TCSANOW, &_original);
    }

    RawMouseMode(RawMouseMode const&) = delete;
    RawMouseMode& operator=(RawMouseMode const&) = delete;
    RawMouseMode(RawMouseMode&&) = delete;
    RawMouseMode& operator=(RawMouseMode&&) = delete;

  private:
    termios _original {};
};

// --- 3D scene ---------------------------------------------------------------------------------

constexpr int GridN = 22;       // surface resolution
constexpr double DomainR = 1.6; // |re|,|im| domain half-width
constexpr int CenterX = 400;
// Place the scene's centre about two-thirds down the 480-pixel-tall canvas, so the surface sits
// lower on the page (its visual centre near one-third up from the bottom) for a better viewing angle.
constexpr int CenterY = 320;
constexpr double Scale = 150.0;

struct Vec3
{
    double x, y, z;
};

double surfaceHeight(double re, double im)
{
    auto const w = (std::complex<double> { re, im } * std::complex<double> { re, im })
                   - std::complex<double> { 1.0, 0.0 };
    return std::abs(w); // |z^2 - 1|
}

/// Projects a world point to screen pixels after applying yaw and pitch.
std::pair<int, int> project(Vec3 p, double yaw, double pitch)
{
    // Yaw about the vertical axis, then pitch about the horizontal axis.
    auto const cx = std::cos(yaw);
    auto const sx = std::sin(yaw);
    auto const x1 = (p.x * cx) - (p.y * sx);
    auto const y1 = (p.x * sx) + (p.y * cx);
    auto const z1 = p.z;

    auto const cy = std::cos(pitch);
    auto const sy = std::sin(pitch);
    auto const y2 = (y1 * cy) - (z1 * sy);
    auto const z2 = (y1 * sy) + (z1 * cy);

    // A touch of perspective by the rotated depth y2.
    auto const depth = 1.0 / (1.0 + (0.15 * y2));
    auto const screenX = CenterX + static_cast<int>(x1 * Scale * depth);
    auto const screenY = CenterY - static_cast<int>(z2 * Scale * depth);
    return { screenX, screenY };
}

std::string moveTo(std::pair<int, int> p)
{
    return "P[" + std::to_string(p.first) + "," + std::to_string(p.second) + "]";
}

std::string lineTo(std::pair<int, int> p)
{
    return "V[" + std::to_string(p.first) + "," + std::to_string(p.second) + "]";
}

std::string render(double yaw, double pitch)
{
    std::string r = "S(E)";

    // Axes (blue) through the origin.
    r += "W(I1)";
    r += moveTo(project({ .x = -1.8, .y = 0, .z = 0 }, yaw, pitch))
         + lineTo(project({ .x = 1.8, .y = 0, .z = 0 }, yaw, pitch));
    r += moveTo(project({ .x = 0, .y = -1.8, .z = 0 }, yaw, pitch))
         + lineTo(project({ .x = 0, .y = 1.8, .z = 0 }, yaw, pitch));
    r += moveTo(project({ .x = 0, .y = 0, .z = 0 }, yaw, pitch))
         + lineTo(project({ .x = 0, .y = 0, .z = 2.2 }, yaw, pitch));

    // Precompute the surface points and their projected pixels.
    std::array<std::array<Vec3, GridN>, GridN> world {};
    for (auto i = 0; i < GridN; ++i)
        for (auto j = 0; j < GridN; ++j)
        {
            auto const re = -DomainR + (2.0 * DomainR * i / (GridN - 1));
            auto const im = -DomainR + (2.0 * DomainR * j / (GridN - 1));
            world[i][j] = Vec3 { .x = re, .y = im, .z = surfaceHeight(re, im) * 0.5 };
        }

    // Wireframe: connect each point to its +i and +j neighbours, colouring by height.
    for (auto i = 0; i < GridN; ++i)
        for (auto j = 0; j < GridN; ++j)
        {
            auto const heightColor = 2 + static_cast<int>(world[i][j].z * 3.0) % 6;
            r += "W(I" + std::to_string(heightColor) + ")";
            if (i + 1 < GridN)
                r += moveTo(project(world[i][j], yaw, pitch)) + lineTo(project(world[i + 1][j], yaw, pitch));
            if (j + 1 < GridN)
                r += moveTo(project(world[i][j], yaw, pitch)) + lineTo(project(world[i][j + 1], yaw, pitch));
        }

    r += "W(I7)P[20,460]T(S1)'|z^2 - 1|  -  drag to rotate, q to quit'";
    return r;
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    RawMouseMode const rawMode;

    auto yaw = 0.6;
    auto pitch = 0.5;
    regis(render(yaw, pitch));

    auto dragging = false;
    auto lastX = 0;
    auto lastY = 0;
    std::string pending;

    while (running)
    {
        std::array<char, 64> buf {};
        auto const n = ::read(STDIN_FILENO, buf.data(), buf.size());
        if (n <= 0)
            break;
        pending.append(buf.data(), static_cast<size_t>(n));

        auto redraw = false;
        size_t pos = 0;
        while (pos < pending.size())
        {
            auto const c = pending[pos];
            if (c == 'q')
            {
                running = 0;
                break;
            }
            if (c == '\033' && pos + 2 < pending.size() && pending[pos + 1] == '[' && pending[pos + 2] == '<')
            {
                // SGR mouse: ESC [ < b ; x ; y (M|m)
                auto const end = pending.find_first_of("Mm", pos + 3);
                if (end == std::string::npos)
                    break; // incomplete; wait for more bytes
                auto const body = pending.substr(pos + 3, end - (pos + 3));
                auto b = 0;
                auto x = 0;
                auto y = 0;
                if (std::sscanf(body.c_str(), "%d;%d;%d", &b, &x, &y) == 3)
                {
                    auto const isRelease = pending[end] == 'm';
                    auto const button = b & 0x43; // low button bits (ignore the motion flag 0x20)
                    if (!isRelease && button == 0 && (b & 0x20) == 0)
                    {
                        dragging = true; // left-button press
                        lastX = x;
                        lastY = y;
                    }
                    else if (isRelease)
                        dragging = false;
                    else if (dragging)
                    {
                        yaw += (x - lastX) * 0.08;
                        pitch += (y - lastY) * 0.08;
                        lastX = x;
                        lastY = y;
                        redraw = true;
                    }
                }
                pos = end + 1;
                continue;
            }
            ++pos;
        }
        pending.erase(0, pos);

        if (redraw && running)
            regis(render(yaw, pitch));
    }

    regis("S(E)");
    writeToTTY("\033[24H\n");
    return 0;
}
