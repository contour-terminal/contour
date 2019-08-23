/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal/Generator.h>
#include <terminal/OutputHandler.h>
#include <terminal/Parser.h>
#include <terminal/Process.h>
#include <terminal/Screen.h>
#include <terminal/Terminal.h>
#include <terminal/Util.h>

#include <fmt/format.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <thread>

#include <cstddef>

#if defined(__unix__)
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#elif defined(_MSC_VER)
#include <Windows.h>
using ssize_t = SSIZE_T;
#endif

using std::placeholders::_1;
using fmt::format;

using namespace std;
namespace {
    string getErrorString()
    {
#if defined(__unix__)
        return strerror(errno);
#else
        DWORD errorMessageID = GetLastError();
        if (errorMessageID == 0)
            return "";

        LPSTR messageBuffer = nullptr;
        size_t size = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorMessageID,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)& messageBuffer,
            0,
            nullptr
        );

        string message(messageBuffer, size);

        LocalFree(messageBuffer);

        return message;
#endif
    }
} // anonymous namespace

enum class Mode {
    PassThrough,
    Proxy,
    Redraw,
};

auto const envvars = terminal::Process::Environment{
    {"TERM", "xterm-256color"},
    {"COLORTERM", "xterm"},
    {"COLORFGBG", "15;0"},
    {"LINES", ""},
    {"COLUMNS", ""},
    {"TERMCAP", ""}
};

class ProxyTerm {
  public:
    ProxyTerm(Mode mode,
              terminal::WindowSize const& windowSize,
              string shell = terminal::Process::loginShell())
        : mode_{mode},
#if defined(__unix__)
          tio_{setupTerminalSettings(STDIN_FILENO)},
#endif
          logger_{ofstream{"trace.log", ios::trunc}},
          terminal_{
              windowSize.columns,
              windowSize.rows,
              bind(&ProxyTerm::screenReply, this, _1),
              [this](auto const& msg) { log("terminal: {}", msg); },
              bind(&ProxyTerm::onStdout, this, _1)
          },
          pty_{ windowSize },
          process_{ pty_, shell, {shell}, envvars },
          inputThread_{ bind(&ProxyTerm::inputThread, this) },
          outputThread_{ bind(&ProxyTerm::outputThread, this) }
    {
        // TODO: when outside term changes windows size, propagate it into here too.
        // TODO: query current cursor position and initialize cursor in internal screen to it OR reset outside screen, too
        log("Forwarder-Mode: {}", static_cast<int>(mode_));
    }

    void join()
    {
        inputThread_.join();
        outputThread_.join();
    }

    ~ProxyTerm()
    {
        // restore some settings
        terminal::Generator generator{ [this](char const* s, size_t n) { writeToConsole(s, n); } };
        generator(terminal::SetMode{ terminal::Mode::VisibleCursor, true });

        // restore flags upon exit
#if defined(__unix__)
        tcsetattr(STDIN_FILENO, TCSANOW, &tio_);
#endif
    }

  private:
    void inputThread()
    {
        for (;;)
        {
            char buf[4096];
            ssize_t const n = readFromConsole(buf, sizeof(buf));
            if (n == -1)
                log("inputThread: read failed. {}", getErrorString());
            else
            {
                log("inputThread: input data: {}", terminal::escape(buf, next(buf, n)));
                ssize_t nwritten = 0;
                while (nwritten < n)
                {
                    auto rv = pty_.write(buf + nwritten, n - nwritten);
                    if (rv == -1)
                        log("inputThread: failed to write to PTY. {}", getErrorString());
                    else
                        nwritten += rv;
                }
            }
        }
    }

    void outputThread()
    {
        enableConsoleVT();
        for (;;)
        {
            char buf[4096];
            if (size_t const n = pty_.read(buf, sizeof(buf)); n > 0)
            {
                log("outputThread.data: {}", terminal::escape(buf, buf + n));
                terminal_.write(buf, n);
                if (mode_ == Mode::PassThrough)
                    writeToConsole(buf, n);
            }
        }
    }

    ssize_t readFromConsole(char* _buf, size_t _size)
    {
#if defined(__unix__)
        return ::read(STDIN_FILENO, _buf, _size);
#else
        DWORD size{ static_cast<DWORD>(_size) };
        DWORD nread{};
        if (ReadFile(GetStdHandle(STD_INPUT_HANDLE), _buf, size, &nread, nullptr))
            return nread;
        else
            return -1;
#endif
    }

    void writeToConsole(char const* _buf, size_t _size)
    {
#if defined(__unix__)
        ::write(STDOUT_FILENO, _buf, _size);
#else
        DWORD nwritten{};
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), _buf, static_cast<DWORD>(_size), &nwritten, nullptr);
#endif
    }

    void enableConsoleVT()
    {
#if defined(_MSC_VER)
        HANDLE hConsole = {GetStdHandle(STD_OUTPUT_HANDLE)};
        DWORD consoleMode{};
        GetConsoleMode(hConsole, &consoleMode);
        consoleMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (!SetConsoleMode(hConsole, consoleMode))
            throw runtime_error{"Could not enable Console VT processing. " + getErrorString()};
#endif
    }

    void onStdout(vector<terminal::Command> const& commands)
    {
        auto const generated = terminal::Generator::generate(commands);

        // log("create: {}", terminal::escape(generated));
        // for (terminal::Command const& command: commands)
        //     log("command: {}", to_string(command));

        switch (mode_)
        {
            case Mode::Proxy:
                writeToConsole(generated.data(), generated.size());
                break;
            case Mode::Redraw:
                redraw();
                break;
            case Mode::PassThrough:
                break;
        }
    }

    // PoC-style naive implementation of a full screen redraw
    void redraw()
    {
        terminal::Generator generator{[this](char const* s, size_t n) { writeToConsole(s, n); }};

        generator(terminal::SetMode{terminal::Mode::VisibleCursor, false});
        generator(terminal::SetMode{terminal::Mode::AutoWrap, false});
        generator(terminal::SetGraphicsRendition{terminal::GraphicsRendition::Reset});

        terminal_.screen().render(
            [&](auto row, auto col, terminal::Screen::Cell const& cell) {
                generator(terminal::MoveCursorTo{row, col});
                generator(terminal::SetForegroundColor{cell.attributes.foregroundColor});
                generator(terminal::SetBackgroundColor{cell.attributes.backgroundColor});

                // TODO: styles

                if (cell.character)
                    generator(terminal::AppendChar{cell.character});
                else
                    generator(terminal::AppendChar{' '}); // FIXME workaround to ensure it's drawn
            }
        );

        // position cursor
        generator(terminal::MoveCursorTo{terminal_.screen().currentRow(),
                                         terminal_.screen().currentColumn()});

        // (TODO: make visible ONLY if meant to be visible)
        generator(terminal::SetMode{terminal::Mode::VisibleCursor, true});
    }

    void screenReply(string_view const& message)
    {
        pty_.write(message.data(), message.size());
    }

    void log(string_view const& msg)
    {
        if (logger_)
            *logger_ << msg << endl;
    }

    template <typename... Args>
    void log(string_view const& msg, Args&&... args)
    {
        if (logger_)
            *logger_ << fmt::format(msg, forward<Args>(args)...) << endl;
    }

#if defined(__unix__)
    static termios getTerminalSettings(int fd)
    {
        termios tio;
        tcgetattr(STDIN_FILENO, &tio);
        return tio;
    }

    static pair<termios, termios> constructTerminalSettings(int fd)
    {
        auto const save = getTerminalSettings(fd);
        auto tio = save;

        // input flags
        tio.c_iflag |= IGNBRK;    // Ignore Break condition on input.
        tio.c_iflag &= ~IXON;     // Disable CTRL-S / CTRL-Q on output.
        tio.c_iflag &= ~IXOFF;    // Disable CTRL-S / CTRL-Q on input.
        tio.c_iflag &= ~ICRNL;    // Ensure CR isn't translated to NL.
        tio.c_iflag &= ~INLCR;    // Ensure NL isn't translated to CR.
        tio.c_iflag &= ~IGNCR;    // Ensure CR isn't ignored.
        tio.c_iflag &= ~IMAXBEL;  // Ensure beeping on full input buffer isn't enabled.
        tio.c_iflag &= ~ISTRIP;   // Ensure stripping of 8th bit on input isn't enabled.

        // output flags
        tio.c_oflag &= ~OPOST;   // Don't enable implementation defined output processing.
        tio.c_oflag &= ~ONLCR;   // Don't map NL to CR-NL.
        tio.c_oflag &= ~OCRNL;   // Don't map CR to NL.
        tio.c_oflag &= ~ONLRET;  // Don't output CR.

        // control flags

        // local flags
        tio.c_lflag &= ~IEXTEN;  // Don't enable implementation defined input processing.
        tio.c_lflag &= ~ICANON;  // Don't enable line buffering (Canonical mode).
        tio.c_lflag &= ~ECHO;    // Don't echo input characters.
        tio.c_lflag &= ~ISIG;    // Don't generate signal upon receiving characters for
                                 // INTR, QUIT, SUSP, DSUSP.

        // special characters
        tio.c_cc[VMIN] = 1;   // Report as soon as 1 character is available.
        tio.c_cc[VTIME] = 0;  // Disable timeout (no need).

        return {tio, save};
    }

    static termios setupTerminalSettings(int fd)
    {
        auto const [tio, save] = constructTerminalSettings(fd);

        if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) == 0)
            tcflush(STDIN_FILENO, TCIOFLUSH);

        return save;
    }
#endif

  private:
    Mode const mode_;
#if defined(__unix__)
    termios tio_;
#endif
    optional<ofstream> logger_;
    terminal::Terminal terminal_;
    terminal::PseudoTerminal pty_;
    terminal::Process process_;
    thread inputThread_;
    thread outputThread_;
    string generated_{};

};

int main(int argc, char const* argv[])
{
    auto windowSize = terminal::currentWindowSize();
    cout << "Host Window Size: " << windowSize.columns << "x" << windowSize.rows << endl;

    auto p = ProxyTerm{Mode::Redraw, windowSize};
    p.join();
}
