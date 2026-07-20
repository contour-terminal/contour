// SPDX-License-Identifier: Apache-2.0

/// DEC Locator event watcher.
///
/// Enables DEC Locator reporting (DECELR) and prints incoming
/// DECLRP reports to the terminal. Useful for interactively testing
/// DEC Locator support.
///
/// Press 'q' or Ctrl+C to quit.

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#ifdef __APPLE__
    #include <util.h>
#else
    #include <termios.h>
#endif

#include <fcntl.h>
#include <unistd.h>

using namespace std::string_view_literals;

namespace
{

bool running = true;

void signalHandler(int signo)
{
    running = false;
    signal(signo, SIG_DFL);
}

void writeToTTY(std::string_view s) noexcept
{
    ::write(STDOUT_FILENO, s.data(), s.size());
}

/// Parses a DECLRP report from raw input.
/// Format: CSI Pe ; Pb ; Pr ; Pc ; Pp & w
struct LocatorReport
{
    int event = 0;  ///< 0=unavailable, 1=request, 2=down, 3=up, 4=outside filter
    int button = 0; ///< Button state bitmask
    int row = 0;    ///< Row (1-based)
    int col = 0;    ///< Column (1-based)
    int page = 0;   ///< Page number
};

enum class ParseState : std::uint8_t
{
    Normal,
    Escape,
    CSI,
    Params,
};

/// Names the DECLRP event code Pe, for human-readable output.
/// @param event The Pe parameter of a DECLRP report.
/// @return A short description of the event, or "unknown" for codes DEC does not define.
[[nodiscard]] std::string_view locatorEventName(int event) noexcept
{
    switch (event)
    {
        case 0: return "unavailable";
        case 1: return "request response";
        case 2: return "button DOWN";
        case 3: return "button UP";
        case 4: return "outside filter";
        default: return "unknown";
    }
}

/// Decodes the parameter string of a DECLRP report into its five fields.
///
/// A DECLRP report reaches us as `CSI Pe ; Pb ; Pr ; Pc ; Pp & w`. This function takes just the
/// `Pe;Pb;Pr;Pc;Pp` text -- everything between the CSI introducer and the `&w` terminator -- and
/// assigns the parameters positionally: the first is the event, the second the button state, then
/// row, column and page.
///
/// Parameters may be empty (`1;;3`) or simply absent (a report may stop early); in both cases the
/// DEC Locator protocol says the parameter takes its default value of 0, which is what the
/// value-initialized LocatorReport already holds.
///
/// @param params The semicolon-separated parameter text, without introducer or terminator.
/// @return The decoded report, with any parameter not present left at its 0 default.
[[nodiscard]] LocatorReport parseLocatorReport(std::string const& params)
{
    auto report = LocatorReport {};
    auto pos = size_t { 0 };
    auto paramIdx = 0;

    while (pos <= params.size() && paramIdx < 5)
    {
        auto const delim = params.find(';', pos);
        auto const token = params.substr(pos, delim == std::string::npos ? std::string::npos : delim - pos);
        auto const val = token.empty() ? 0 : std::stoi(token);

        switch (paramIdx)
        {
            case 0: report.event = val; break;
            case 1: report.button = val; break;
            case 2: report.row = val; break;
            case 3: report.col = val; break;
            case 4: report.page = val; break;
            default: break; // DECLRP defines no further parameters; ignore them.
        }

        ++paramIdx;
        if (delim == std::string::npos)
            break;
        pos = delim + 1;
    }

    return report;
}

} // namespace

int main()
{
    // Save terminal settings and enter raw mode
    termios savedTermios {};
    tcgetattr(STDIN_FILENO, &savedTermios);

    auto tio = savedTermios;
    tio.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &tio);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Enable DEC Locator: CSI 1 ; 2 ' z  (continuous mode, character cell coords)
    writeToTTY("\033[1;2'z");
    // Select all events: CSI 1 ; 3 ' {  (button down + button up)
    writeToTTY("\033[1;3'{");
    // Hide cursor
    writeToTTY("\033[?25l");

    writeToTTY("DEC Locator event watcher. Press 'q' to quit.\r\n\r\n");

    auto state = ParseState::Normal;
    auto params = std::string {};
    auto intermediate = char {};

    while (running)
    {
        char buf[128];
        auto const n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0)
            continue;

        for (auto i = 0; i < n; ++i)
        {
            auto const ch = buf[i];

            switch (state)
            {
                case ParseState::Normal:
                    if (ch == '\033')
                        state = ParseState::Escape;
                    else if (ch == 'q' || ch == 'Q')
                        running = false;
                    break;

                case ParseState::Escape:
                    if (ch == '[')
                    {
                        state = ParseState::CSI;
                        params.clear();
                        intermediate = 0;
                    }
                    else
                        state = ParseState::Normal;
                    break;

                case ParseState::CSI:
                    if (ch >= '0' && ch <= '9')
                    {
                        params += ch;
                        state = ParseState::Params;
                    }
                    else
                        state = ParseState::Normal;
                    break;

                case ParseState::Params:
                    if ((ch >= '0' && ch <= '9') || ch == ';')
                        params += ch;
                    else if (ch == '&')
                        intermediate = '&';
                    else if (ch == 'w' && intermediate == '&')
                    {
                        // A complete DECLRP report has arrived: CSI Pe;Pb;Pr;Pc;Pp & w
                        auto const report = parseLocatorReport(params);

                        writeToTTY(
                            std::format("\r  DECLRP: event={} ({}) button={} row={} col={} page={}\033[K\r\n",
                                        report.event,
                                        locatorEventName(report.event),
                                        report.button,
                                        report.row,
                                        report.col,
                                        report.page));

                        state = ParseState::Normal;
                    }
                    else
                        state = ParseState::Normal;
                    break;
            }
        }
    }

    // Disable DEC Locator: CSI 0 ' z
    writeToTTY("\033[0'z");
    // Show cursor
    writeToTTY("\033[?25h");
    writeToTTY("\r\nTerminating.\r\n");

    tcsetattr(STDIN_FILENO, TCSANOW, &savedTermios);

    return 0;
}
