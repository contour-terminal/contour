// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/EchoChild.hpp>

#include <vtbackend/testing/Workloads.hpp>

#include <algorithm>
#include <cstdlib>
#include <ranges>
#include <string>

#include <vthost/latency/EchoProtocol.hpp>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <cerrno>

    #include <termios.h>
    #include <unistd.h>
#endif

namespace vthost::latency
{

namespace
{
    /// How much a flood child writes per write, as coreutils' cat does.
    constexpr auto FloodWriteSize = std::size_t { 128 } * 1024;
    /// How much workload a flood child generates once and writes over and over.
    constexpr auto FloodBufferSize = std::size_t { 1 } * 1024 * 1024;

#ifdef _WIN32
    /// Puts the console into raw VT mode: each key as it comes, as a VT sequence, and escape sequences
    /// interpreted on output.
    [[nodiscard]] bool enterRawMode()
    {
        auto const input = GetStdHandle(STD_INPUT_HANDLE);
        auto const output = GetStdHandle(STD_OUTPUT_HANDLE);
        auto outputMode = DWORD { 0 };
        if (!GetConsoleMode(output, &outputMode))
            return false;
        return SetConsoleMode(input, ENABLE_VIRTUAL_TERMINAL_INPUT)
               && SetConsoleMode(output, outputMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    void writeAll(std::string_view bytes)
    {
        auto const output = GetStdHandle(STD_OUTPUT_HANDLE);
        while (!bytes.empty())
        {
            auto written = DWORD { 0 };
            if (!WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
                return;
            bytes.remove_prefix(written);
        }
    }

    /// @return The next byte of input; nullopt once the input ended.
    [[nodiscard]] std::optional<char> readByte()
    {
        auto byte = char { 0 };
        auto read = DWORD { 0 };
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), &byte, 1, &read, nullptr) || read == 0)
            return std::nullopt;
        return byte;
    }
#else
    [[nodiscard]] bool enterRawMode()
    {
        auto attributes = termios {};
        if (tcgetattr(STDIN_FILENO, &attributes) != 0)
            return false;
        cfmakeraw(&attributes);
        return tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == 0;
    }

    void writeAll(std::string_view bytes)
    {
        while (!bytes.empty())
        {
            auto const written = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return;
            bytes.remove_prefix(static_cast<std::size_t>(written));
        }
    }

    /// @return The next byte of input; nullopt once the input ended.
    [[nodiscard]] std::optional<char> readByte()
    {
        auto byte = char { 0 };
        while (true)
        {
            auto const count = ::read(STDIN_FILENO, &byte, 1);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return std::nullopt;
            return byte;
        }
    }
#endif
} // namespace

int runEchoChild()
{
    if (!enterRawMode())
        return EXIT_FAILURE;
    writeAll(PromptText);
    auto state = EchoState {};
    while (auto const byte = readByte())
    {
        auto const reply = echoReply(state, *byte);
        if (reply.action == EchoAction::Quit)
            break;
        writeAll(reply.bytes);
    }
    return EXIT_SUCCESS;
}

int runFloodChild(std::string_view workload, std::chrono::seconds duration)
{
    auto const row =
        std::ranges::find(vtbackend::test::Workloads, workload, &vtbackend::test::Workload::name);
    if (row == std::ranges::end(vtbackend::test::Workloads) || !enterRawMode())
        return EXIT_FAILURE;
    auto const buffer = vtbackend::test::makeWorkload(*row, FloodBufferSize);
    auto const deadline = std::chrono::steady_clock::now() + duration;
    auto written = std::size_t { 0 };
    auto offset = std::size_t { 0 };
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto const chunk = std::string_view(buffer).substr(offset, FloodWriteSize);
        writeAll(chunk);
        written += chunk.size();
        offset = offset + chunk.size() >= buffer.size() ? 0 : offset + chunk.size();
    }
    writeAll("\r\n" + std::string(FloodEndMarker) + std::to_string(written) + std::string(FloodEndTerminator)
             + "\r\n");
    // Stays until told to end, like the echo child: a session whose application is gone is gone with its
    // screen, and the screen is where the measuring side reads the count.
    while (auto const byte = readByte())
        if (*byte == QuitByte)
            break;
    return EXIT_SUCCESS;
}

} // namespace vthost::latency
