// SPDX-License-Identifier: Apache-2.0

/// @file hud-screen-demo.cpp
/// Demonstrates the HUD (Heads-Up Display) overlay screen (DEC mode 2035).
///
/// Draws background content on the primary screen, then enables the HUD overlay
/// and displays a colored popup with an animated progress bar that fills from 0% to 100%.
/// Once the simulated job completes, the HUD is disabled and the primary screen is intact.

#include <sys/ioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <termios.h>
#include <unistd.h>

using namespace std::literals;
using namespace std::chrono_literals;

namespace
{

// -- Low-level terminal helpers -----------------------------------------------

void writeToTTY(std::string_view s) noexcept
{
    ::write(STDOUT_FILENO, s.data(), s.size());
}

/// Reads a terminal response up to (and including) the given @p sentinel character.
std::string readResponse(char sentinel, std::chrono::milliseconds timeout = 500ms)
{
    std::string response;
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline)
    {
        char ch {};
        auto const n = ::read(STDIN_FILENO, &ch, 1);
        if (n == 1)
        {
            response += ch;
            if (ch == sentinel)
                break;
        }
    }
    return response;
}

/// Queries DECRQM for the given DEC mode number and returns the mode value.
/// Returns 1 = set, 2 = reset, 0 = unknown/not-recognized, -1 = timeout.
int queryDECMode(unsigned mode)
{
    // Send DECRQM: CSI ? <mode> $ p
    auto const request = std::format("\033[?{}$p", mode);
    writeToTTY(request);

    // Expected response: CSI ? <mode> ; <value> $ y
    auto const response = readResponse('y');
    if (response.empty())
        return -1;

    // Parse ";X$y" at the end — X is the mode value digit.
    auto const semi = response.rfind(';');
    if (semi == std::string::npos || semi + 1 >= response.size())
        return -1;

    return response[semi + 1] - '0';
}

// -- RAII terminal raw mode ---------------------------------------------------

struct RawMode
{
    termios saved {};

    RawMode()
    {
        ::tcgetattr(STDIN_FILENO, &saved);
        auto raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1; // 100 ms read timeout for non-blocking reads.
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    ~RawMode() { ::tcsetattr(STDIN_FILENO, TCSANOW, &saved); }

    RawMode(RawMode const&) = delete;
    RawMode& operator=(RawMode const&) = delete;
};

// -- Drawing helpers ----------------------------------------------------------

/// Moves the cursor to (row, col) — 1-based.
std::string moveTo(int row, int col)
{
    return std::format("\033[{};{}H", row, col);
}

/// Sets foreground and background colors using SGR (256-color mode).
std::string setColor(int fg, int bg)
{
    return std::format("\033[38;5;{}m\033[48;5;{}m", fg, bg);
}

constexpr auto Reset = "\033[0m"sv;
constexpr auto Bold = "\033[1m"sv;
constexpr auto HideCursor = "\033[?25l"sv;
constexpr auto ShowCursor = "\033[?25h"sv;

// -- Popup / progress bar drawing ---------------------------------------------

constexpr int PopupWidth = 40;
constexpr int PopupHeight = 7;
constexpr int BarWidth = PopupWidth - 6; // 34 usable cells for the bar

/// Repeats a UTF-8 string @p s exactly @p count times.
std::string repeat(std::string_view s, int count)
{
    std::string result;
    result.reserve(s.size() * static_cast<size_t>(count));
    for (auto i = 0; i < count; ++i)
        result += s;
    return result;
}

/// Draws the popup frame on the HUD screen centered at the given origin.
void drawPopupFrame(int topRow, int leftCol)
{
    // Colors: white text on a dark blue background.
    auto const color = setColor(15, 17);
    auto const hLine = repeat("─", PopupWidth - 2);

    // Top border
    writeToTTY(std::format("{}{}{}┌{}┐", moveTo(topRow, leftCol), color, Bold, hLine));

    // Middle rows (empty body)
    for (auto row = 1; row < PopupHeight - 1; ++row)
    {
        writeToTTY(std::format("{}│{}│", moveTo(topRow + row, leftCol), std::string(PopupWidth - 2, ' ')));
    }

    // Bottom border
    writeToTTY(std::format("{}└{}┘", moveTo(topRow + PopupHeight - 1, leftCol), hLine));

    // Title
    auto constexpr Title = " Processing... "sv;
    auto const titleCol = leftCol + (PopupWidth - static_cast<int>(Title.size())) / 2;
    writeToTTY(std::format("{}{}", moveTo(topRow, titleCol), Title));

    writeToTTY(std::format("{}", Reset));
}

/// Draws the progress bar at the given percentage (0–100).
void drawProgressBar(int topRow, int leftCol, int percent)
{
    auto const barRow = topRow + 3;
    auto const barCol = leftCol + 3;

    auto const filled = std::clamp(percent * BarWidth / 100, 0, BarWidth);
    auto const empty = BarWidth - filled;

    // Green filled portion, dark gray empty portion.
    writeToTTY(moveTo(barRow, barCol));
    writeToTTY(setColor(16, 40)); // black on green
    writeToTTY(std::string(static_cast<size_t>(filled), ' '));
    writeToTTY(setColor(250, 236)); // light gray on dark gray
    writeToTTY(std::string(static_cast<size_t>(empty), ' '));
    writeToTTY(std::string(Reset));

    // Percentage label
    auto const label = std::format("{:>3}%", percent);
    auto const labelCol = leftCol + (PopupWidth - static_cast<int>(label.size())) / 2;
    writeToTTY(std::format("{}{}{}{}{}", moveTo(topRow + 5, labelCol), setColor(15, 17), Bold, label, Reset));
}

} // namespace

int main()
{
    if (!::isatty(STDIN_FILENO))
    {
        std::cerr << "Error: stdin is not a terminal.\n";
        return EXIT_FAILURE;
    }

    auto const rawMode = RawMode {};
    writeToTTY(HideCursor);

    // -- Feature detection via DECRQM -----------------------------------------
    auto const modeState = queryDECMode(2035);
    if (modeState != 1 && modeState != 2)
    {
        writeToTTY(ShowCursor);
        std::cerr << "Error: DEC mode 2035 (HUD screen) is not supported by this terminal.\n";
        return EXIT_FAILURE;
    }

    // -- Draw background on primary screen ------------------------------------
    // Query terminal size via TIOCGWINSZ.
    struct winsize ws {};
    ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    auto const rows = static_cast<int>(ws.ws_row);
    auto const cols = static_cast<int>(ws.ws_col);

    std::this_thread::sleep_for(500ms);

    // -- Enable HUD overlay ---------------------------------------------------
    writeToTTY("\033[?2035h");

    // Center the popup.
    auto const topRow = (rows - PopupHeight) / 2 + 1;
    auto const leftCol = (cols - PopupWidth) / 2 + 1;

    drawPopupFrame(topRow, leftCol);

    // -- Animate progress bar from 0% to 100% --------------------------------
    for (auto percent = 0; percent <= 100; ++percent)
    {
        drawProgressBar(topRow, leftCol, percent);
        std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(400ms);

    // -- Disable HUD overlay — popup disappears, primary screen intact --------
    writeToTTY("\033[?2035l");

    writeToTTY(ShowCursor);
    writeToTTY(std::format("{}", moveTo(rows, 1)));
    writeToTTY("\n");

    return EXIT_SUCCESS;
}
