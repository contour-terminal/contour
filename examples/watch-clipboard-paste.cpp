// SPDX-License-Identifier: Apache-2.0

/// @file watch-clipboard-paste.cpp
///
/// Example application demonstrating the Binary Paste Mode (DEC mode 2033).
///
/// This program enables both bracketed text paste (mode 2004) and binary paste (mode 2033),
/// then listens on stdin for paste events.
///
/// - Text paste: prints the pasted text to stdout.
/// - Binary paste: saves the binary data to ~/Downloads/binary-paste-<timestamp>.<ext>
///
/// Terminate with Ctrl+D.

#include <vtbackend/Functions.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/primitives.h>

#include <vtparser/Parser.h>
#include <vtparser/ParserEvents.h>

#include <vtpty/UnixUtils.h>

#include <crispy/base64.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#if defined(__APPLE__)
    #include <util.h>
#else
    #include <termios.h>
#endif

#include <fcntl.h>
#include <unistd.h>

using namespace std;
using namespace vtbackend;

namespace
{

/// Returns a file extension for a given MIME type, or "bin" for unknown types.
constexpr string_view extensionForMimeType(string_view mimeType) noexcept
{
    if (mimeType == "image/png")
        return "png";
    if (mimeType == "image/jpeg")
        return "jpg";
    if (mimeType == "image/gif")
        return "gif";
    if (mimeType == "image/bmp")
        return "bmp";
    if (mimeType == "image/svg+xml")
        return "svg";
    return "bin";
}

/// Generates a timestamp string suitable for filenames (YYYYMMDD-HHMMSS).
string makeTimestamp()
{
    auto const now = chrono::system_clock::now();
    auto const time = chrono::system_clock::to_time_t(now);
    auto const tm = *localtime(&time);
    return format("{:04}{:02}{:02}-{:02}{:02}{:02}",
                  tm.tm_year + 1900,
                  tm.tm_mon + 1,
                  tm.tm_mday,
                  tm.tm_hour,
                  tm.tm_min,
                  tm.tm_sec);
}

struct PasteWatcher final: public vtparser::NullParserEvents
{
    static bool _running;

    termios savedTermios;
    vtparser::Parser<vtparser::ParserEvents> vtInputParser;

    // State for bracketed paste collection
    bool inBracketedPaste = false;
    string bracketedPasteBuffer;

    // State for DCS binary paste collection
    bool inDcsBinaryPaste = false;
    unsigned dcsBinarySize = 0;
    string dcsDataString;

    // State for tracking CSI sequences
    Sequence _sequence {};
    SequenceParameterBuilder _parameterBuilder;

    PasteWatcher() noexcept:
        savedTermios { vtpty::util::getTerminalSettings(STDIN_FILENO) },
        vtInputParser { *this },
        _parameterBuilder(_sequence.parameters())
    {
        // Set raw mode so we receive all bytes directly
        auto tio = savedTermios;
        tio.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        vtpty::util::applyTerminalSettings(STDIN_FILENO, tio);

        // Enable bracketed paste (mode 2004) and binary paste (mode 2033)
        writeToTTY("\033[?2004h"); // bracketed paste
        writeToTTY("\033[?2033h"); // binary paste

        // Configure MIME preferences: DCS 2033 b c<mime-list> ST
        // Accept images and HTML, in priority order.
        writeToTTY("\033P2033bcimage/png,image/jpeg,image/gif,image/bmp,image/svg+xml,text/html\033\\");

        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);

        writeToTTY("Binary Paste Watcher\r\n");
        writeToTTY("====================\r\n");
        writeToTTY("Modes enabled: bracketed paste (2004), binary paste (2033)\r\n");
        writeToTTY(
            "MIME preferences: image/png, image/jpeg, image/gif, image/bmp, image/svg+xml, text/html\r\n");
        writeToTTY("Paste text or images from your clipboard.\r\n");
        writeToTTY("Press Ctrl+D to exit.\r\n\r\n");
    }

    ~PasteWatcher() override
    {
        writeToTTY("\033[?2033l"); // disable binary paste
        writeToTTY("\033[?2004l"); // disable bracketed paste
        vtpty::util::applyTerminalSettings(STDIN_FILENO, savedTermios);
        writeToTTY("\r\nTerminating.\r\n");
    }

    static void signalHandler(int signo)
    {
        _running = false;
        signal(signo, SIG_DFL);
    }

    void writeToTTY(string_view s) noexcept { ::write(STDOUT_FILENO, s.data(), s.size()); }

    int run()
    {
        while (_running)
            processInput();
        return EXIT_SUCCESS;
    }

    void processInput()
    {
        char buf[4096];
        auto const n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0)
            vtInputParser.parseFragment(string_view(buf, static_cast<size_t>(n)));
        else if (n == 0)
            _running = false; // EOF
    }

    // --- Parser event handlers ---

    void execute(char controlCode) override
    {
        if (controlCode == 0x04) // Ctrl+D (EOT)
            _running = false;
    }

    void print(char32_t ch) override
    {
        if (inBracketedPaste)
        {
            // Collect pasted text between CSI 200~ and CSI 201~ delimiters.
            // Encode the codepoint as UTF-8 into the buffer.
            char buf[4];
            if (ch < 0x80)
            {
                buf[0] = static_cast<char>(ch);
                bracketedPasteBuffer.append(buf, 1);
            }
            else if (ch < 0x800)
            {
                buf[0] = static_cast<char>(0xC0 | (ch >> 6));
                buf[1] = static_cast<char>(0x80 | (ch & 0x3F));
                bracketedPasteBuffer.append(buf, 2);
            }
            else if (ch < 0x10000)
            {
                buf[0] = static_cast<char>(0xE0 | (ch >> 12));
                buf[1] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                buf[2] = static_cast<char>(0x80 | (ch & 0x3F));
                bracketedPasteBuffer.append(buf, 3);
            }
            else
            {
                buf[0] = static_cast<char>(0xF0 | (ch >> 18));
                buf[1] = static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                buf[2] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                buf[3] = static_cast<char>(0x80 | (ch & 0x3F));
                bracketedPasteBuffer.append(buf, 4);
            }
        }
    }

    size_t print(std::string_view text, size_t /*columnsUsed*/) override
    {
        if (inBracketedPaste)
            bracketedPasteBuffer.append(text);
        return 0;
    }

    // CSI sequence handling for bracketed paste delimiters
    void clear() noexcept override
    {
        _sequence.clearExceptParameters();
        _parameterBuilder.reset();
    }

    void collectLeader(char leader) noexcept override { _sequence.setLeader(leader); }

    void collect(char ch) override { _sequence.intermediateCharacters().push_back(ch); }

    void paramDigit(char ch) noexcept override
    {
        _parameterBuilder.multiplyBy10AndAdd(static_cast<uint8_t>(ch - '0'));
    }

    void param(char ch) noexcept override
    {
        switch (ch)
        {
            case ';': _parameterBuilder.nextParameter(); break;
            case ':': _parameterBuilder.nextSubParameter(); break;
            default:
                if (ch >= '0' && ch <= '9')
                    _parameterBuilder.multiplyBy10AndAdd(static_cast<uint8_t>(ch - '0'));
                break;
        }
    }

    void dispatchCSI(char finalChar) override
    {
        _sequence.setCategory(FunctionCategory::CSI);
        _sequence.setFinalChar(finalChar);
        _parameterBuilder.fixiate();

        // Check for bracketed paste start: CSI 200 ~
        if (finalChar == '~' && _sequence.parameterCount() >= 1)
        {
            auto const param0 = _sequence.param(0);
            if (param0 == 200) // Start of bracketed paste
            {
                inBracketedPaste = true;
                bracketedPasteBuffer.clear();
                return;
            }
            if (param0 == 201) // End of bracketed paste
            {
                inBracketedPaste = false;
                handleTextPaste(bracketedPasteBuffer);
                bracketedPasteBuffer.clear();
                return;
            }
        }

        _sequence.clear();
    }

    // DCS handling for binary paste: DCS 2033 ; <size> b d<mime>;<base64> ST
    void hook(char finalChar) override
    {
        _parameterBuilder.fixiate();

        if (finalChar == 'b' && _sequence.parameterCount() >= 1 && _sequence.param(0) == 2033)
        {
            inDcsBinaryPaste = true;
            dcsBinarySize = _sequence.parameterCount() >= 2 ? static_cast<unsigned>(_sequence.param(1)) : 0;
            dcsDataString.clear();
            if (dcsBinarySize > 0)
                dcsDataString.reserve(dcsBinarySize * 4 / 3 + 100); // base64 size estimate + mime header
        }
    }

    void put(char ch) override
    {
        if (inDcsBinaryPaste)
            dcsDataString += ch;
        else if (inBracketedPaste)
            bracketedPasteBuffer += ch;
    }

    void unhook() override
    {
        if (inDcsBinaryPaste)
        {
            inDcsBinaryPaste = false;

            if (!dcsDataString.empty())
            {
                auto const subCommand = dcsDataString.front();
                auto const payload = string_view(dcsDataString).substr(1);

                switch (subCommand)
                {
                    case 'd': // Data delivery sub-command
                        handleBinaryPaste(payload);
                        break;
                    default:
                        writeToTTY(format("[Binary Paste] Unknown sub-command: '{}'\r\n", subCommand));
                        break;
                }
            }

            dcsDataString.clear();
        }
    }

    // --- Paste handlers ---

    void handleTextPaste(string_view text)
    {
        writeToTTY(format("[Text Paste] {} bytes:\r\n", text.size()));
        // Write line by line, adding CR before each LF for terminal display
        for (auto const ch: text)
        {
            if (ch == '\n')
                writeToTTY("\r\n"sv);
            else
            {
                auto const c = static_cast<char>(ch);
                ::write(STDOUT_FILENO, &c, 1);
            }
        }
        writeToTTY("\r\n---\r\n"sv);
    }

    void handleBinaryPaste(string_view dataString)
    {
        // Parse data delivery payload: <mime-type> ; <base64-data>
        auto const semicolonPos = dataString.find(';');
        if (semicolonPos == string_view::npos)
        {
            writeToTTY("[Binary Paste] Error: malformed data string (no semicolon separator)\r\n"sv);
            return;
        }

        auto const mimeType = dataString.substr(0, semicolonPos);
        auto const base64Data = dataString.substr(semicolonPos + 1);

        // Decode base64
        auto const decoded = crispy::base64::decode(base64Data);

        // Validate size: if Ps was provided and non-zero, the decoded size must match.
        if (dcsBinarySize > 0 && decoded.size() != dcsBinarySize)
        {
            writeToTTY(format("[Binary Paste] Error: size mismatch (declared {} bytes, decoded {} bytes). "
                              "Discarding.\r\n",
                              dcsBinarySize,
                              decoded.size()));
            return;
        }

        auto const ext = extensionForMimeType(mimeType);
        auto const timestamp = makeTimestamp();

        // Build output path: ~/Downloads/binary-paste-<timestamp>.<ext>
        auto const homeDir = string(getenv("HOME") ? getenv("HOME") : "/tmp");
        auto const downloadsDir = filesystem::path(homeDir) / "Downloads";
        filesystem::create_directories(downloadsDir);

        auto const filename = format("binary-paste-{}.{}", timestamp, ext);
        auto const outputPath = downloadsDir / filename;

        // Write to file
        ofstream file(outputPath, ios::binary);
        if (file.is_open())
        {
            file.write(decoded.data(), static_cast<streamsize>(decoded.size()));
            file.close();
            writeToTTY(format("[Binary Paste] MIME: {}, {} bytes -> {}\r\n",
                              mimeType,
                              decoded.size(),
                              outputPath.string()));
        }
        else
        {
            writeToTTY(format("[Binary Paste] Error: could not write to {}\r\n", outputPath.string()));
        }
    }
};

bool PasteWatcher::_running = true;

} // namespace

int main()
{
    auto watcher = PasteWatcher {};
    return watcher.run();
}
