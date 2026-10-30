// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/CaptureScreen.hpp>

#include <vtbackend/vt/Functions.hpp>

#include <vtparser/Parser.hpp>
#include <vtparser/ParserEvents.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>

#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// clang-format off
#ifdef _WIN32
    #include <Windows.h>
#else
    #include <fcntl.h>
    #include <sys/select.h>
    #include <termios.h>
    #include <unistd.h>
#endif
// clang-format on

#ifndef STDIN_FILENO
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

namespace contour::cli
{

namespace
{
    class CaptureBufferCollector: public vtparser::NullParserEvents
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
            auto const [code, offset] = vtparser::extractCodePrefix(capturedBuffer);
            if (code == vtbackend::CaptureBufferCode)
            {
                auto const payload =
                    string_view(capturedBuffer.data() + offset, capturedBuffer.size() - offset);
                if (splitByWord)
                {
                    core::split(payload, ' ', [&](auto word) -> bool {
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

#ifndef _WIN32
    /// Opens the controlling terminal for reading and writing.
    ///
    /// Kept out of TTY's constructor body so the descriptor can be a default member initializer:
    /// an assignment in the body would have to sit inside `#ifndef _WIN32`, and hoisting it into
    /// the member-initializer list -- which is what cppcoreguidelines-prefer-member-initializer
    /// asks for -- would place it outside that guard, where neither open() nor `fd` exists.
    ///
    /// @return The file descriptor for /dev/tty, or -1 if it could not be opened.
    [[nodiscard]] int openControllingTerminal() noexcept
    {
        return open("/dev/tty", O_RDWR);
    }
#endif

#ifdef _WIN32
    /// The console standard input is.
    class StdinConsole final: public ConsoleInput
    {
      public:
        [[nodiscard]] std::optional<unsigned long> mode() const override
        {
            auto modes = DWORD {};
            if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &modes) == 0)
                return std::nullopt;
            return modes;
        }

        [[nodiscard]] std::expected<void, unsigned long> setMode(unsigned long mode) override
        {
            if (SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), mode) == 0)
                return std::unexpected(GetLastError());
            return {};
        }
    };
#endif

    struct TTY final: CaptureTransport
    {
        bool configured = false;
#ifndef _WIN32
        int fd = openControllingTerminal();
        termios savedModes {};
#else
        DWORD savedModes {};
#endif

        ~TTY() override
        {
#ifndef _WIN32
            tcsetattr(fd, TCSANOW, &savedModes);
#else
            if (!configured)
                return;
            auto console = StdinConsole {};
            leaveCaptureInputMode(console, savedModes, cerr);
#endif
        }

        TTY()
        {
#ifndef _WIN32
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
            auto console = StdinConsole {};
            if (auto const saved = enterCaptureInputMode(console))
            {
                savedModes = *saved;
                configured = true;
            }
#endif
        }

        int wait(std::chrono::microseconds& timeout) override
        {
#ifdef _WIN32
            auto const fd0 = GetStdHandle(STD_INPUT_HANDLE);
            auto const timeoutMillis =
                static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
            DWORD const result = WaitForSingleObject(fd0, timeoutMillis);
            switch (result)
            {
                case WAIT_OBJECT_0: return 1;
                case WAIT_TIMEOUT: return 0;
                case WAIT_FAILED:
                case WAIT_ABANDONED:
                default: return -1;
            }
#else
            fd_set sin;
            fd_set sout;
            fd_set serr;
            FD_ZERO(&sin);
            FD_ZERO(&sout);
            FD_ZERO(&serr);
            FD_SET(fd, &sin);
            auto const watermark = fd + 1;
            auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
            auto remaining = timeval {};
            remaining.tv_sec = static_cast<decltype(remaining.tv_sec)>(seconds.count());
            remaining.tv_usec = static_cast<decltype(remaining.tv_usec)>((timeout - seconds).count());
            auto const result = select(watermark, &sin, &sout, &serr, &remaining);
            timeout =
                std::chrono::seconds { remaining.tv_sec } + std::chrono::microseconds { remaining.tv_usec };
            return result;
#endif
        }

        int write(char const* buf, size_t size) const
        {
#ifdef _WIN32
            DWORD nwritten {};
            if (WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, static_cast<DWORD>(size), &nwritten, nullptr))
                return static_cast<int>(nwritten);
            else
                return -1;
#else
            return static_cast<int>(::write(fd, buf, size));
#endif
        }

        int write(string_view text) override { return write(text.data(), text.size()); }

        int read(void* buf, size_t size) override
        {
#ifdef _WIN32
            DWORD nread {};
            if (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, static_cast<DWORD>(size), &nread, nullptr))
                return static_cast<int>(nread);
            else
                return -1;
#else
            return static_cast<int>(::read(fd, buf, size));
#endif
        }
    };

    /// Queries the peer terminal's screen size over @p input (CSI 18 t / CSI 8;lines;cols t reply).
    optional<tuple<int, int>> queryScreenSize(CaptureTransport& input, std::chrono::microseconds& timeout)
    {
        // Naive implementation. TODO: use select() to poll and time out properly.
        if (input.write("\033[18t") < 0)
            return nullopt;

        // Consume reply: `CSI 8 ; <LINES> ; <COLUMNS> t`
        string reply;
        for (;;)
        {
            if (input.wait(timeout) <= 0)
                return nullopt;

            char ch {};
            if (input.read(&ch, sizeof(ch)) != sizeof(ch))
                return nullopt;

            if (ch == 't')
                break;

            reply.push_back(ch);
        }

        auto const screenSizeReply = core::split(reply, ';');
        if (screenSizeReply.size() != 3)
            return nullopt;

        auto const columns = stoi(string(screenSizeReply.at(1)));
        auto const lines = stoi(string(screenSizeReply.at(2)));
        return tuple { columns, lines };
    }

    // Reads a response chunk.
    bool readCaptureReply(CaptureTransport& input,
                          std::chrono::microseconds& timeout,
                          bool words,
                          ostream& output)
    {
        auto captureBufferCollector = CaptureBufferCollector { output, words };
        auto parser = vtparser::Parser<vtparser::ParserEvents> { captureBufferCollector };

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
                cerr << "Time out. VTE did not respond to CAPTURE `CSI > Ps ; Ps , t`.\r\n";
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

#ifdef _WIN32
std::optional<unsigned long> enterCaptureInputMode(ConsoleInput& console)
{
    auto const saved = console.mode();
    if (!saved)
        return nullopt;

    auto modes = *saved;
    modes |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    modes &= ~static_cast<unsigned long>(ENABLE_LINE_INPUT);
    modes &= ~static_cast<unsigned long>(ENABLE_ECHO_INPUT);
    if (!console.setMode(modes))
        return nullopt;
    return saved;
}

void leaveCaptureInputMode(ConsoleInput& console, unsigned long saved, ostream& diagnostics)
{
    if (auto const restored = console.setMode(saved); !restored)
        diagnostics << std::format(
            "Could not restore the console's input mode (error {}): it is left without line editing or "
            "echo.\r\n",
            restored.error());
}
#endif

bool captureScreen(CaptureSettings const& settings, CaptureTransport& transport, ostream& output)
{
    auto timeout = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double> { settings.timeout });

    auto const screenSizeOpt = queryScreenSize(transport, timeout);
    if (!screenSizeOpt.has_value())
    {
        cerr << "Could not get current screen size.\r\n";
        return false;
    }
    auto const [numColumns, numLines] = screenSizeOpt.value();

    if (settings.verbosityLevel > 0)
        cerr << std::format("Screen size: {}x{}. Capturing lines {} ({}) to file {}.\r\n",
                            numColumns,
                            numLines,
                            settings.lineMode,
                            settings.lineCount,
                            settings.words ? "words" : "lines",
                            settings.outputFile);

    // request screen capture
    //
    // The ',' intermediate is what distinguishes XTCAPTURE from xterm's XTSMTITLE, which owns the bare
    // `CSI > Ps ; Ps t` this used to send. @see vtbackend/Functions.h, XTCAPTURE.
    if (transport.write(
            std::format("\033[>{};{},t", std::to_underlying(settings.lineMode), settings.lineCount))
        < 0)
    {
        cerr << "Could not request screen capture.\r\n";
        return false;
    }

    return readCaptureReply(transport, timeout, settings.words, output);
}

bool captureScreen(CaptureSettings const& settings)
{
    auto tty = TTY {};
    if (!tty.configured)
        return false;

    reference_wrapper<ostream> output(cout);
    unique_ptr<ostream> customOutput;
    if (settings.outputFile != "-"sv)
    {
        customOutput = make_unique<ofstream>(
            core::pathFromUtf8(settings.outputFile).value_or(std::filesystem::path {}), std::ios::trunc);
        output = *customOutput;
    }

    return captureScreen(settings, tty, output);
}

} // namespace contour::cli
