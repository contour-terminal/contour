// SPDX-License-Identifier: Apache-2.0
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include <termios.h>
#include <unistd.h>

using namespace std::literals;

static bool processEvent(std::string_view response)
{
    if (response == "\033[?997;1n")
    {
        std::cout << "dark\n";
        return true;
    }
    else if (response == "\033[?997;2n")
    {
        std::cout << "light\n";
        return true;
    }

    std::cout << "unknown\n";
    return false;
}

static bool queryDarkLightModeOnce()
{
    // Also send DA1 to detect end of reply, in case the terminal does not support color mode detection
    std::cout << "\033[?996n\033[c";
    std::cout.flush();

    char buf[32];
    size_t n = 0;
    size_t i = 0;
    while (i < sizeof(buf))
    {
        if (read(STDIN_FILENO, buf + i, 1) != 1)
            break;
        else if (buf[i] == 'n')
            n = i + 1;
        else if (buf[i] == 'c')
        {
            ++i;
            break;
        }
        ++i;
    }

    return processEvent(std::string_view(buf, n));
}

static void signalHandler(int signo)
{
    std::signal(signo, SIG_DFL);
    std::cerr << "Received signal " << signo << ", exiting...\n";
}

static void monitorDarkLightModeChanges()
{
    char buf[32];
    size_t n = 0;
    size_t i = 0;
    while (true)
    {
        if (i >= sizeof(buf))
            i = 0;
        if (read(STDIN_FILENO, buf + i, 1) != 1)
            break;
        else if (buf[i] == '\033')
        {
            buf[0] = '\033';
            i = 1;
        }
        else if (buf[i] == 'n')
        {
            n = i + 1;
            auto const response = std::string_view(buf, n);
            processEvent(response);
        }
        ++i;
    }
}

int main(int argc, char* argv[])
{
    if (!isatty(STDIN_FILENO))
    {
        std::cerr << "stdin is not a terminal\n";
        return EXIT_FAILURE;
    }

    termios oldt {};
    termios newt {};
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    if (argc == 2 && (argv[1] == "-h"sv || argv[1] == "--help"sv))
    {
        std::cout << "Usage: " << argv[0] << " [monitor]\n";
        return EXIT_SUCCESS;
    }

    if (!queryDarkLightModeOnce())
        return EXIT_FAILURE;

    if (argc == 2 && argv[1] == "monitor"sv)
    {
        struct sigaction sa;
        sa.sa_handler = signalHandler;
        sa.sa_flags = 0; // Explicitly don't set SA_RESTART;
        sigemptyset(&sa.sa_mask);
        for (int const sig: { SIGTERM, SIGINT, SIGQUIT })
            sigaction(sig, &sa, nullptr);

        std::cerr << "Monitoring dark/light mode changes, press Ctrl+C to exit...\n";
        std::cerr << "\033[?2031h"; // Enable dark/light mode change notifications
        std::cerr.flush();
        monitorDarkLightModeChanges();
        std::cerr << "\033[?2031l"; // Disable dark/light mode change notifications
        std::cerr << "Finished monitoring dark/light mode changes\n";
        std::cerr.flush();
    }

    // restore terminal
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    return EXIT_SUCCESS;
}
