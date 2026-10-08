// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.hpp>

#include <crispy/BufferObject.hpp>

#include <chrono>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>

namespace vtpty::testing
{

/// Reads @p pty until @p done holds for everything read so far, for at most about 30 seconds.
/// @param pty  The PTY to read.
/// @param done Whether what was read so far is enough.
/// @return Everything read.
inline std::string readUntil(Pty& pty, std::function<bool(std::string_view)> const& done)
{
    constexpr auto ReadSize = std::size_t { 4096 };
    auto pool = crispy::BufferObjectPool<char> { ReadSize };
    auto collected = std::string {};
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 600))
    {
        if (done(collected))
            break;
        auto const storage = pool.allocateBufferObject();
        if (auto const result = pty.read(*storage, std::chrono::milliseconds { 50 }, ReadSize);
            result && !result->data.empty())
            collected.append(result->data);
    }
    return collected;
}

/// @return Whether @p pty printed @p needle within about 30 seconds.
inline bool printed(Pty& pty, std::string_view needle)
{
    return readUntil(pty, [needle](std::string_view text) { return text.contains(needle); }).contains(needle);
}

} // namespace vtpty::testing
