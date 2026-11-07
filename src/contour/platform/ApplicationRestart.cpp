// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ApplicationRestart.hpp>

#include <vtpty/WindowsCommandLine.hpp>

#include <algorithm>
#include <cstdint>
#include <ranges>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <Windows.h>
#endif

namespace contour::platform
{

namespace
{
    /// The length of UTF-8 text in UTF-16 code units: one per code point, two for those past the BMP (which
    /// UTF-8 spells with a four-byte sequence). Counts lead bytes, so it needs no conversion that could fail.
    [[nodiscard]] std::size_t utf16Length(std::string_view utf8) noexcept
    {
        constexpr auto ContinuationMask = uint8_t { 0b1100'0000 };
        constexpr auto ContinuationTag = uint8_t { 0b1000'0000 };
        constexpr auto FourByteMask = uint8_t { 0b1111'1000 };
        constexpr auto FourByteTag = uint8_t { 0b1111'0000 };
        auto units = std::size_t { 0 };
        for (auto const byte: utf8 | std::views::transform([](char c) { return static_cast<uint8_t>(c); }))
        {
            if ((byte & ContinuationMask) == ContinuationTag)
                continue;
            units += (byte & FourByteMask) == FourByteTag ? 2 : 1;
        }
        return units;
    }

    /// Registers nothing, where no operating system restarts applications.
    class NoApplicationRestart final: public ApplicationRestart
    {
      public:
        [[nodiscard]] std::expected<void, std::error_code> registerRestart(
            std::string_view /*commandLine*/) override
        {
            return {};
        }
    };

#ifdef _WIN32
    /// Restart Manager's registration, through RegisterApplicationRestart().
    class RestartManagerRestart final: public ApplicationRestart
    {
      public:
        [[nodiscard]] std::expected<void, std::error_code> registerRestart(
            std::string_view commandLine) override
        {
            auto wide = core::toUtf16(commandLine);
            if (!wide)
                return std::unexpected(wide.error());
            // Not when the application crashed or hung, nor when the system restarts after an update: the
            // restart this is for is the installer's own, after it replaced the files.
            constexpr auto Flags = DWORD { RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT };
            auto const result = RegisterApplicationRestart(wide->empty() ? nullptr : wide->c_str(), Flags);
            if (FAILED(result))
                return std::unexpected(std::error_code { static_cast<int>(result), std::system_category() });
            return {};
        }
    };
#endif
} // namespace

std::span<std::string const> restartArguments(std::span<std::string const> commandLine)
{
    return commandLine.subspan(std::min<std::size_t>(1, commandLine.size()));
}

std::string restartCommandLine(std::span<std::string const> arguments)
{
    auto commandLine = vtpty::buildWindowsArguments(arguments);
    if (utf16Length(commandLine) > MaxRestartCommandLine)
        return {};
    return commandLine;
}

std::unique_ptr<ApplicationRestart> makeApplicationRestart()
{
#ifdef _WIN32
    return std::make_unique<RestartManagerRestart>();
#else
    return std::make_unique<NoApplicationRestart>();
#endif
}

} // namespace contour::platform
