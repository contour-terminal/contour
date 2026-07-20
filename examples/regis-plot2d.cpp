// SPDX-License-Identifier: Apache-2.0

/// @file regis-plot2d.cpp
/// Plots a 2D mathematical function (a damped sine wave) with labelled axes and a grid, using
/// Contour's ReGIS graphics. It shows how to map data coordinates to the ReGIS addressing space and
/// draw a polyline with the Vector (V) command.

#include <cmath>
#include <ranges>
#include <string>
#include <string_view>

#include <unistd.h>

using namespace std::string_view_literals;

namespace
{

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

// The plotting rectangle inside the 800x480 ReGIS canvas.
constexpr int Left = 80;
constexpr int Right = 760;
constexpr int Top = 40;
constexpr int Bottom = 430;

// The data domain and range.
constexpr double XMin = 0.0;
constexpr double XMax = 12.0;
constexpr double YMin = -1.0;
constexpr double YMax = 1.0;

// Grid spacing, expressed as a step size plus the number of grid lines it yields.
// Counting integer steps (rather than accumulating a double) keeps the loop exact.
constexpr double XGridStep = 2.0;
constexpr double YGridStep = 0.5;
constexpr int XGridLines = static_cast<int>((XMax - XMin) / XGridStep) + 1;
constexpr int YGridLines = static_cast<int>((YMax - YMin) / YGridStep) + 1;

int mapX(double x)
{
    return Left + static_cast<int>((x - XMin) / (XMax - XMin) * (Right - Left));
}

int mapY(double y)
{
    // Y grows downward in ReGIS, so the top of the range maps to the top (smaller) pixel row.
    return Bottom - static_cast<int>((y - YMin) / (YMax - YMin) * (Bottom - Top));
}

double f(double x)
{
    return std::exp(-0.2 * x) * std::sin(2.0 * x);
}

} // namespace

int main()
{
    std::string r = "S(E)";

    // Grid (dotted, dim colour). Vertical lines every 2 units, horizontal every 0.5.
    r += "W(I8)W(P4)";
    for (auto const i: std::views::iota(0, XGridLines))
    {
        auto const gx = XMin + (XGridStep * i);
        r += "P[" + std::to_string(mapX(gx)) + "," + std::to_string(Top) + "]V[" + std::to_string(mapX(gx))
             + "," + std::to_string(Bottom) + "]";
    }
    for (auto const i: std::views::iota(0, YGridLines))
    {
        auto const gy = YMin + (YGridStep * i);
        r += "P[" + std::to_string(Left) + "," + std::to_string(mapY(gy)) + "]V[" + std::to_string(Right)
             + "," + std::to_string(mapY(gy)) + "]";
    }
    r += "W(P1)"; // solid again

    // Axes (bright).
    r += "W(I7)";
    r += "P[" + std::to_string(Left) + "," + std::to_string(mapY(0.0)) + "]V[" + std::to_string(Right) + ","
         + std::to_string(mapY(0.0)) + "]"; // x-axis at y=0
    r += "P[" + std::to_string(Left) + "," + std::to_string(Top) + "]V[" + std::to_string(Left) + ","
         + std::to_string(Bottom) + "]"; // y-axis

    // The function curve as a connected polyline in green.
    r += "W(I3)";
    auto first = true;
    for (auto i = 0; i <= 340; ++i)
    {
        auto const x = XMin + ((XMax - XMin) * (static_cast<double>(i) / 340.0));
        auto const px = mapX(x);
        auto const py = mapY(f(x));
        if (first)
        {
            r += "P[" + std::to_string(px) + "," + std::to_string(py) + "]";
            first = false;
        }
        else
            r += "V[" + std::to_string(px) + "," + std::to_string(py) + "]";
    }

    // Labels.
    r += "W(I7)";
    r += "P[" + std::to_string(Left) + ",20]T(S1)'f(x) = exp(-0.2x) sin(2x)'";
    r += "P[" + std::to_string(Right - 40) + "," + std::to_string(mapY(0.0) + 20) + "]T(S0)'x'";

    regis(r);
    writeToTTY("\033[40H\n");
    return 0;
}
