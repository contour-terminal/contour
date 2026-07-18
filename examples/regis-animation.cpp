// SPDX-License-Identifier: Apache-2.0

/// @file regis-animation.cpp
/// A looping ReGIS animation: an analog clock whose hands sweep around the dial. Each frame is a
/// fresh ReGIS DCS string that erases the canvas (S(E)) and redraws the scene, demonstrating the
/// persistent-context / erase-and-redraw model. Press Ctrl+C to stop.

#include <atomic>
#include <cmath>
#include <csignal>
#include <string>
#include <string_view>

#include <unistd.h>

using namespace std::string_view_literals;

namespace
{

std::atomic<bool> g_running { true };

void onSignal(int) noexcept
{
    g_running = false;
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

constexpr int CenterX = 400;
constexpr int CenterY = 240;

std::string hand(double angleTurns, int length, int colorRegister)
{
    // angleTurns in [0,1); 0 points up (12 o'clock), increasing clockwise.
    auto const radians = (angleTurns * 2.0 * M_PI) - (M_PI / 2.0);
    auto const x = CenterX + static_cast<int>(std::cos(radians) * length);
    auto const y = CenterY + static_cast<int>(std::sin(radians) * length);
    return "W(I" + std::to_string(colorRegister) + ")P[" + std::to_string(CenterX) + ","
           + std::to_string(CenterY) + "]V[" + std::to_string(x) + "," + std::to_string(y) + "]";
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto frame = 0;
    while (g_running)
    {
        std::string r = "S(E)";

        // Dial: a circle plus twelve hour ticks.
        r += "W(I7)P[" + std::to_string(CenterX + 180) + "," + std::to_string(CenterY) + "]C(C)[";
        r += std::to_string(CenterX) + "," + std::to_string(CenterY) + "]";
        for (auto h = 0; h < 12; ++h)
        {
            auto const a = (static_cast<double>(h) / 12.0 * 2.0 * M_PI) - (M_PI / 2.0);
            auto const x0 = CenterX + static_cast<int>(std::cos(a) * 165);
            auto const y0 = CenterY + static_cast<int>(std::sin(a) * 165);
            auto const x1 = CenterX + static_cast<int>(std::cos(a) * 180);
            auto const y1 = CenterY + static_cast<int>(std::sin(a) * 180);
            r += "P[" + std::to_string(x0) + "," + std::to_string(y0) + "]V[" + std::to_string(x1) + ","
                 + std::to_string(y1) + "]";
        }

        // Hands: the second hand sweeps once per 60 frames, the minute hand slowly.
        auto const seconds = static_cast<double>(frame % 60) / 60.0;
        auto const minutes = static_cast<double>(frame % 3600) / 3600.0;
        r += hand(minutes, 110, 3); // minute hand (green)
        r += hand(seconds, 150, 2); // second hand (red)

        r += "P[300,440]T(S1)'ReGIS animation - Ctrl+C to stop'";

        regis(r);

        usleep(100 * 1000); // ~10 fps
        ++frame;
    }

    // Clear and reset on exit.
    regis("S(E)");
    writeToTTY("\033[24H\n");
    return 0;
}
