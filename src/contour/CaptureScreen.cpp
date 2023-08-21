// SPDX-License-Identifier: Apache-2.0
#include <contour/CaptureScreen.h>

#include <vtbackend/Functions.h>

#include <vtparser/Parser.h>
#include <vtparser/ParserEvents.h>

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

// clang-format off
#if defined(_WIN32)
    #include <Winsock2.h>
#else
    #include <fcntl.h>
    #include <sys/select.h>
    #include <termios.h>
    #include <unistd.h>
#endif
// clang-format on

#if !defined(STDIN_FILENO)
    #define STDIN_FILENO 0
#endif

using std::cerr;
using std::cout;
using std::make_unique;
using std::nullopt;
using std::ofstream;
using std::optional;
using std::ostream;
using std::reference_wrapper;
using std::stoi;
using std::streamsize;
using std::string;
using std::string_view;
using std::tuple;
using std::unique_ptr;

using namespace std::string_view_literals;

namespace contour
{

class CaptureBufferCollector: public terminal::NullParserEvents
{
  public:
    std::ostream& output;
    bool splitByWord;
    std::string capturedBuffer;
    bool done = false;

    CaptureBufferCollector(ostream& out, bool words): output { out }, splitByWord { words } {}

    void startPM() override { capturedBuffer.clear(); }

    void putPM(char t) override { capturedBuffer += t; }
    void execute(char ch) override { putPM(ch); }

    void dispatchPM() override
    {
        auto const [code, offset] = terminal::parser::extractCodePrefix(capturedBuffer);
        if (code == terminal::CaptureBufferCode)
        {
            auto const payload = string_view(capturedBuffer.data() + offset, capturedBuffer.size() - offset);
            if (splitByWord)
            {
                crispy::split(payload, ' ', [&](auto word) -> bool {
                    output.write(word.data(), static_cast<streamsize>(word.size()));
                    output << '\n';
                    return true;
                });
            }
            else
                output.write(payload.data(), static_cast<streamsize>(payload.size()));
            if (payload.empty())
                done = true;
        }
    }
};

namespace
{
    struct TTY
    {
        bool configured = false;
#if !defined(_WIN32)
        int fd = -1;
        termios savedModes {};
#else
        DWORD savedModes {};
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
                cerr << "Could not open current terminal.\r\n";
                return;
            }

            if (tcgetattr(fd, &savedModes) < 0)
                return;

            // disable buffered input
            termios tio = savedModes;
            tio.c_lflag &= (tcflag_t) ~ICANON;  // disable canonical input
            tio.c_lflag &= (tcflag_t) ~ECHO;    // disable echoing
            tio.c_iflag &= (tcflag_t) ~IMAXBEL; // disable bell on full input buffer
            tio.c_iflag &= (tcflag_t) ~ISTRIP;  // disable stripping of 8th bit on input

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

        int wait(timeval* timeout) const
        {
#if defined(_WIN32)
            auto const fd0 = GetStdHandle(STD_INPUT_HANDLE);
            DWORD const timeoutMillis = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
            DWORD const result = WaitForSingleObject(fd0, timeoutMillis);
            switch (result)
            {
                case WSA_WAIT_EVENT_0: return 1;
                case WSA_WAIT_TIMEOUT: return 0;
                case WAIT_FAILED:
                case WAIT_ABANDONED:
                default: return -1;
            }
#else
            fd_set sin, sout, serr;
            FD_ZERO(&sin);
            FD_ZERO(&sout);
            FD_ZERO(&serr);
            FD_SET(fd, &sin);
            auto const watermark = fd + 1;
            return select(watermark, &sin, &sout, &serr, timeout);
#endif
        }

        int write(char const* buf, size_t size)
        {
#if defined(_WIN32)
            DWORD nwritten {};
            if (WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, static_cast<DWORD>(size), &nwritten, nullptr))
                return static_cast<int>(nwritten);
            else
                return -1;
#else
            return static_cast<int>(::write(fd, buf, size));
#endif
        }

        int write(string_view text) { return write(text.data(), text.size()); }

        int read(void* buf, size_t size)
        {
#if defined(_WIN32)
            DWORD nread {};
            if (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, static_cast<DWORD>(size), &nread, nullptr))
                return static_cast<int>(nread);
            else
                return -1;
#else
            return static_cast<int>(::read(fd, buf, size));
#endif
        }

        optional<tuple<int, int>> screenSize(timeval* timeout)
        {
            // Naive implementation. TODO: use select() to poll and time out properly.
            write("\033[18t");

            // Consume reply: `CSI 8 ; <LINES> ; <COLUMNS> t`
            string reply;
            for (;;)
            {
                if (wait(timeout) <= 0)
                    return nullopt;

                char ch {};
                if (read(&ch, sizeof(ch)) != sizeof(ch))
                    return nullopt;

                if (ch == 't')
                    break;

                reply.push_back(ch);
            }

            auto const screenSizeReply = crispy::split(reply, ';');
            if (screenSizeReply.size() != 3)
                return nullopt;

            auto const columns = stoi(string(screenSizeReply.at(1)));
            auto const lines = stoi(string(screenSizeReply.at(2)));
            return tuple { columns, lines };
        }
    };

    // Reads a response chunk.
    bool readCaptureReply(TTY& input, timeval* timeout, bool words, ostream& output)
    {
        auto captureBufferCollector = CaptureBufferCollector { output, words };
        auto parser = terminal::parser::Parser<terminal::ParserEvents> { captureBufferCollector };

        // Response is of format: PM 314 ; <screen capture> ST`
        while (true)
        {
            int rv = input.wait(timeout);
            if (rv < 0)
            {
                perror("select");
                return false;
            }
            else if (rv == 0)
            {
                cerr << "Time out. VTE did not respond to CAPTURE `CSI > Ps ; Ps t`.\r\n";
                return false;
            }

            char buf[4096];
            rv = input.read(buf, sizeof(buf));
            if (rv < 0)
            {
                perror("read");
                return false;
            }

            auto const inputView = string_view(buf, static_cast<size_t>(rv));
            parser.parseFragment(inputView);

            if (captureBufferCollector.done)
                return true;
        }
    }
} // namespace

bool captureScreen(CaptureSettings const& settings)
{
    auto tty = TTY {};
    if (!tty.configured)
        return false;

    auto constexpr MicrosPerSecond = 1'000'000;
    auto const timeoutMicros = int(settings.timeout * MicrosPerSecond);
    auto timeout = timeval {};
    timeout.tv_sec = timeoutMicros / MicrosPerSecond;
    timeout.tv_usec = timeoutMicros % MicrosPerSecond;

    auto const screenSizeOpt = tty.screenSize(&timeout);
    if (!screenSizeOpt.has_value())
    {
        cerr << "Could not get current screen size.\r\n";
        return false;
    }
    auto const [numColumns, numLines] = screenSizeOpt.value();

    if (settings.verbosityLevel > 0)
        cerr << fmt::format("Screen size: {}x{}. Capturing lines {} ({}) to file {}.\r\n",
                            numColumns,
                            numLines,
                            settings.logicalLines ? "logical" : "physical",
                            settings.lineCount,
                            settings.words ? "words" : "lines",
                            settings.outputFile.data());

    // request screen capture
    reference_wrapper<ostream> output(cout);
    unique_ptr<ostream> customOutput;
    if (settings.outputFile != "-"sv)
    {
        customOutput = make_unique<ofstream>(settings.outputFile.data(), std::ios::trunc);
        output = *customOutput;
    }

    tty.write(fmt::format("\033[>{};{}t", settings.logicalLines ? '1' : '0', settings.lineCount));

    return readCaptureReply(tty, &timeout, settings.words, output);
}

} // namespace contour
