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
#include <contour/CaptureScreen.h>

#include <crispy/utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>

#if defined(_WIN32)
    #include <Winsock2.h>
    #include <Windows.h>
#else
    #include <sys/select.h>
    #include <termios.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

#if !defined(STDIN_FILENO)
    #define STDIN_FILENO 0
#endif

using std::cerr;
using std::copy_n;
using std::cout;
using std::make_unique;
using std::nullopt;
using std::ofstream;
using std::optional;
using std::ostream;
using std::reference_wrapper;
using std::stoi;
using std::string;
using std::string_view;
using std::tuple;
using std::unique_ptr;

using namespace std::string_view_literals;

namespace contour {

namespace
{
    struct TTY
    {
        bool configured = false;
#if !defined(_WIN32)
        int fd = -1;
        termios savedModes{};
#else
        DWORD savedModes{};
#endif

        ~TTY()
        {
#if !defined(_WIN32)
            tcsetattr(fd, TCSANOW, &savedModes);
#else
            auto stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
            SetConsoleMode(stdinHandle, savedModes);
#endif
        }

        TTY()
        {
#if !defined(_WIN32)
            fd = open("/dev/tty", O_RDWR);
            if (fd < 0)
            {
                cerr << "Could not open current terminal.\n";
                return;
            }

            if (tcgetattr(fd, &savedModes) < 0)
                return;

            // disable buffered input
            termios tio = savedModes;
            tio.c_lflag &= ~(ICANON | ECHO);

            // {{{ in case I want to do more with this
            // input flags
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
            // }}}

            if (tcsetattr(fd, TCSANOW, &tio) < 0)
                return;

            configured = true;
#else
            auto stdinHandle = GetStdHandle(STD_INPUT_HANDLE);

            GetConsoleMode(stdinHandle, &savedModes);

            DWORD modes = savedModes;
            modes |= ENABLE_VIRTUAL_TERMINAL_INPUT;
            modes &= ~ENABLE_LINE_INPUT;
            modes &= ~ENABLE_ECHO_INPUT;

            SetConsoleMode(stdinHandle, modes);
            configured = true;
#endif
        }

        int wait(timeval* _timeout)
        {
#if defined(_WIN32)
            auto const fd0 = GetStdHandle(STD_INPUT_HANDLE);
            DWORD const timeoutMillis = _timeout->tv_sec * 1000 + _timeout->tv_usec / 1000;
            DWORD const result = WaitForSingleObject(fd0, timeoutMillis);
            switch (result)
            {
                case WSA_WAIT_EVENT_0:
                    return 1;
                case WSA_WAIT_TIMEOUT:
                    return 0;
                case WAIT_FAILED:
                case WAIT_ABANDONED:
                default:
                    return -1;
            }
#else
            fd_set sin, sout, serr;
            FD_ZERO(&sin);
            FD_ZERO(&sout);
            FD_ZERO(&serr);
            FD_SET(fd, &sin);
            auto const watermark = fd + 1;
            return select(watermark, &sin, &sout, &serr, _timeout);
#endif
        }

        int write(char const* _buf, size_t _size)
        {
#if defined(_WIN32)
            DWORD nwritten{};
            if (WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), _buf, static_cast<DWORD>(_size), &nwritten, nullptr))
                return static_cast<int>(nwritten);
            else
                return -1;
#else
            return ::write(fd, _buf, _size);
#endif
        }

        int write(string_view _text)
        {
            return write(_text.data(), _text.size());
        }

        int read(void* _buf, size_t _size)
        {
#if defined(_WIN32)
            DWORD nread{};
            if (ReadFile(GetStdHandle(STD_INPUT_HANDLE), _buf, static_cast<DWORD>(_size), &nread, nullptr))
                return static_cast<int>(nread);
            else
                return -1;
#else
            return ::read(fd, _buf, _size);
#endif
        }

        optional<tuple<int, int>> screenSize(timeval* _timeout)
        {
            // Naive implementation. TODO: use select() to poll and time out properly.
            write("\033[18t");

            // Consume reply: `CSI 8 ; <LINES> ; <COLUMNS> t`
            string reply;
            for (;;)
            {
                if (wait(_timeout) <= 0)
                    return nullopt;

                char ch{};
                if (read(&ch, sizeof(ch)) != sizeof(ch))
                    return nullopt;

                if (ch == 't')
                    break;

                reply.push_back(ch);
            }

            auto const screenSizeReply = crispy::split(reply, ';');
            auto const columns = stoi(string(screenSizeReply.at(1)));
            auto const lines = stoi(string(screenSizeReply.at(2)));

            return tuple{columns, lines};
        }
    };

    auto constexpr ReplyPrefix = "\033]314;"sv; // DCS 314 ;
    auto constexpr ReplySuffix = "\033\\"sv;    // ST

    // Reads a *single* response chunk.
    bool readCaptureChunk(TTY& _input, timeval* _timeout, string& _reply)
    {
        timeval timeout = *_timeout;
        // Response is of format: OSC 314 ; <screen capture> ST`
        long long int n = 0;
        while (true)
        {
            int rv = _input.wait(&timeout);
            if (rv < 0)
            {
                perror("select");
                return false;
            }
            else if (rv == 0)
            {
                cerr << "VTE did not respond to CAPTURE `CSI > Ps ; Ps ; Ps t`.\n";
                return false;
            }

            char buf[4096];
            rv = _input.read(buf, sizeof(buf));
            if (rv < 0)
            {
                perror("read");
                return false;
            }

            copy_n(buf, rv, back_inserter(_reply));

            if (n == 0 && !crispy::startsWith(string_view(_reply), ReplyPrefix))
            {
                cerr << fmt::format("Invalid response from terminal received. Does not start with expected reply prefix.\n");
                return false;
            }
            n++;

            if (!crispy::endsWith(string_view(_reply), ReplySuffix))
                continue;

            return true;
        }
    }
}

bool captureScreen(CaptureSettings const& _settings)
{
    auto tty = TTY{};
    if (!tty.configured)
        return false;

    auto constexpr MicrosPerSecond = 1000000;
    auto const timeoutMicros = int(_settings.timeout * MicrosPerSecond);
    auto timeout = timeval{};
    timeout.tv_sec = timeoutMicros / MicrosPerSecond;
    timeout.tv_usec = timeoutMicros % MicrosPerSecond;

    auto const screenSizeOpt = tty.screenSize(&timeout);
    if (!screenSizeOpt.has_value())
    {
        cerr << "Could not get current screen size.\n";
        return false;
    }
    auto const [numColumns, numLines] = screenSizeOpt.value();

    if (_settings.verbosityLevel > 0)
        cerr << fmt::format("Screen size: {}x{}. Capturing lines {} to file {}.\n",
                            numColumns, numLines,
                            _settings.logicalLines ? "logical" : "physical",
                            _settings.lineCount,
                            _settings.outputFile.data());

    // request screen capture
    string reply;
    reply.reserve(numColumns * std::max(_settings.lineCount, numLines));

    reference_wrapper<ostream> output(cout);
    unique_ptr<ostream> customOutput;
    if (_settings.outputFile != "-"sv)
    {
        //cerr << "Opening output stream: '" << _settings.outputFile << "'\n";
        customOutput = make_unique<ofstream>(_settings.outputFile.data(), std::ios::trunc);
        output = *customOutput;
    }

    tty.write(fmt::format("\033[>{};{}t",
                          _settings.logicalLines ? '1' : '0',
                          _settings.lineCount));

    while (true)
    {
        if (!readCaptureChunk(tty, &timeout, reply))
            return false;

        auto const payload = string_view(reply.data() + ReplyPrefix.size(),
                                         reply.size() - ReplyPrefix.size() - ReplySuffix.size());
        if (payload.empty())
            return true;

        output.get().write(payload.data(), payload.size());
        reply.clear();
    }
}

} // end namespace
