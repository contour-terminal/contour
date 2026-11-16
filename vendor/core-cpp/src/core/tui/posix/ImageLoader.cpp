// SPDX-License-Identifier: Apache-2.0
#include <core/tui/ImageLoader.hpp>

#include <core/Environment.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

/// @file
/// Reading an image out of the desktop clipboard, on POSIX: there is no system call for it, so the
/// tool the session provides -- wl-paste under Wayland, xclip under X11 -- is asked for each media
/// type in turn. In endo this and the Windows answer were the two arms of an `#ifdef` in
/// `src/tui/ImageLoader.cpp` (f774a210).

namespace core::tui
{

namespace
{

    /// @brief Runs a command and captures its stdout as raw bytes.
    ///
    /// @param command Shell command to execute.
    /// @return Raw bytes from stdout, or empty vector on failure.
    auto runCommandAndCapture(char const* command) -> std::vector<std::uint8_t>
    {
        auto* pipe = popen(command, "r");
        if (!pipe)
            return {};

        auto result = std::vector<std::uint8_t>();
        auto buf = std::array<std::uint8_t, 4096>();
        while (auto const bytesRead = std::fread(buf.data(), 1, buf.size(), pipe))
            result.insert(result.end(), buf.data(), buf.data() + bytesRead);

        auto const status = pclose(pipe);
        if (status != 0)
            return {};

        return result;
    }

} // namespace

auto readClipboardImage() -> std::optional<ClipboardImage>
{
    // Try each image format in order of preference.
    struct ClipboardQuery
    {
        char const* waylandCommand;
        char const* x11Command;
        char const* mediaType;
    };

    static constexpr auto Queries = std::array<ClipboardQuery, 3> { {
        { .waylandCommand = "wl-paste --no-newline --type image/png 2>/dev/null",
          .x11Command = "xclip -selection clipboard -target image/png -o 2>/dev/null",
          .mediaType = "image/png" },
        { .waylandCommand = "wl-paste --no-newline --type image/jpeg 2>/dev/null",
          .x11Command = "xclip -selection clipboard -target image/jpeg -o 2>/dev/null",
          .mediaType = "image/jpeg" },
        { .waylandCommand = "wl-paste --no-newline --type image/bmp 2>/dev/null",
          .x11Command = "xclip -selection clipboard -target image/bmp -o 2>/dev/null",
          .mediaType = "image/bmp" },
    } };

    auto const isWayland = core::defaultEnvironment().get("WAYLAND_DISPLAY").has_value();

    for (auto const& query: Queries)
    {
        auto const* command = isWayland ? query.waylandCommand : query.x11Command;
        auto data = runCommandAndCapture(command);
        if (data.empty())
            continue;

        // Verify the data actually looks like the expected image format.
        auto const detected = detectImageMediaType(std::span<std::uint8_t const>(data));
        if (detected.empty())
            continue;

        return ClipboardImage {
            .data = std::move(data),
            .mediaType = detected,
        };
    }

    return std::nullopt;
}

} // namespace core::tui
