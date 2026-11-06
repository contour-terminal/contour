// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.hpp>

#include <crispy/BufferObject.hpp>

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <cstdlib>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace vtpty::testing
{

/// How long readUntil() reads, unless told otherwise.
constexpr auto DefaultReadTimeout = std::chrono::milliseconds { 30'000 };

/// How long one read waits for the PTY to say something.
constexpr auto ReadPollInterval = std::chrono::milliseconds { 50 };

/// What readUntil() waits with: told how long, it lets that much time pass.
using Wait = std::function<void(std::chrono::milliseconds)>;

/// @return A Wait that sleeps, in real time.
inline Wait sleeping()
{
    return [](std::chrono::milliseconds duration) {
        std::this_thread::sleep_for(duration);
    };
}

/// Reads @p pty until @p done holds for everything read so far, until @p timeout has passed, or until the
/// PTY says its far end is gone -- however many reads that takes: a program writing in many small pieces,
/// as a line editor redrawing its prompt does, gets as long as one writing in a few.
/// @param pty     The PTY to read.
/// @param done    Whether what was read so far is enough.
/// @param timeout How long to read for at most.
/// @param clock   What measures @p timeout.
/// @param wait    What waits out the rest of the poll interval after a read that came back early with
///                nothing, which reading again at once would spin on.
/// @return Everything read.
inline std::string readUntil(Pty& pty,
                             std::function<bool(std::string_view)> const& done,
                             std::chrono::milliseconds timeout = DefaultReadTimeout,
                             core::platform::IClock const& clock = core::platform::defaultSteadyClock(),
                             Wait const& wait = sleeping())
{
    constexpr auto ReadSize = std::size_t { 4096 };
    auto pool = crispy::BufferObjectPool<char> { ReadSize };
    auto collected = std::string {};
    auto const deadline = clock.now() + timeout;
    while (!done(collected))
    {
        auto const readStart = clock.now();
        if (readStart >= deadline)
            break;
        auto const storage = pool.allocateBufferObject();
        auto const result = pty.read(*storage, ReadPollInterval, ReadSize);
        auto const error = errno;
        if (result && !result->data.empty())
        {
            collected.append(result->data);
            continue;
        }
        // An end of file, or a failure that is not "nothing yet" (EIO from a PTY whose child has gone, a
        // broken pipe): the far end will say nothing more.
        if (result || (error != EAGAIN && error != EINTR))
            break;
        if (auto const elapsed = clock.now() - readStart; elapsed < ReadPollInterval)
            wait(std::chrono::duration_cast<std::chrono::milliseconds>(ReadPollInterval - elapsed));
    }
    return collected;
}

/// What a terminal answers to the queries a pseudoconsole may send it at startup: primary device attributes
/// and the cursor position. An unanswered one can hold ConPTY's output back.
constexpr auto TerminalAnswers = std::array {
    std::pair { std::string_view { "\x1b[c" }, std::string_view { "\x1b[?61;4c" } },
    std::pair { std::string_view { "\x1b[6n" }, std::string_view { "\x1b[1;1R" } },
};

/// Reads @p pty as readUntil() does, answering each of TerminalAnswers' queries once as a terminal would.
/// @param pty  The PTY to read.
/// @param done Whether what was read so far is enough.
/// @return Everything read.
inline std::string readUntilAnswering(Pty& pty, std::function<bool(std::string_view)> const& done)
{
    auto answered = std::vector<std::string_view> {};
    return readUntil(pty, [&](std::string_view text) {
        for (auto const& [query, answer]: TerminalAnswers)
            if (text.contains(query) && !std::ranges::contains(answered, query))
            {
                answered.push_back(query);
                std::ignore = pty.write(answer);
            }
        return done(text);
    });
}

/// @return Whether @p pty printed @p needle within about 30 seconds.
inline bool printed(Pty& pty, std::string_view needle)
{
    return readUntil(pty, [needle](std::string_view text) { return text.contains(needle); }).contains(needle);
}

#ifndef _WIN32
/// Reads the slave side of a PTY, as a program reading its input does, until @p done holds: what lets a
/// write parked on a program that reads nothing through.
/// @param master The PTY's master side; its slave is opened by name.
/// @param done   Asked before every read.
inline void drainSlave(PtyMasterHandle master, std::function<bool()> const& done)
{
    auto name = std::array<char, 256> {};
    if (::ptsname_r(unbox<int>(master), name.data(), name.size()) != 0)
        return;
    auto const slave = ::open(name.data(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
    auto buffer = std::array<char, 4096> {};
    while (!done())
        if (::read(slave, buffer.data(), buffer.size()) <= 0)
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    ::close(slave);
}
#endif

} // namespace vtpty::testing
