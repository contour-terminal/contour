// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the vthost tests share: a session to host, and command-block records as a host sends them.

#include <vtbackend/shell/CommandBlock.hpp>

#include <vtpty/MockPty.hpp>

#include <crispy/BufferObject.hpp>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <vthost/SessionHost.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost::testing
{

/// A PTY whose application stops reading its input when the test says so: a write then parks until the test
/// drains it -- as a real PTY's write parks once the buffer the application leaves unread is full -- or until
/// the PTY closes. A read parks too, until the reader is woken, so that a session's pump idles on it as on a
/// real PTY rather than spinning, or until the application writes output (sendOutput()), which it reads.
class StallingPty final: public vtpty::MockPty
{
  public:
    using MockPty::MockPty;

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        auto lock = std::unique_lock { _mutex };
        auto const woken = [this] {
            return _pendingWakeups > 0 || _closed || isStdoutDataAvailable();
        };
        if (timeout)
            std::ignore = _wake.wait_for(lock, *timeout, woken);
        else
            _wake.wait(lock, woken);
        if (!_closed && isStdoutDataAvailable())
            return MockPty::read(storage, timeout, size);
        if (_pendingWakeups > 0)
            _pendingWakeups -= 1;
        errno = EAGAIN; // as a real PTY's woken read reports: nothing read, try again
        return std::nullopt;
    }

    [[nodiscard]] int write(std::string_view data) override
    {
        auto lock = std::unique_lock { _mutex };
        _parkedWrites += 1;
        _drained.wait(lock, [this] { return !_stalled || _closed; });
        _parkedWrites -= 1;
        if (_closed)
        {
            errno = EPIPE;
            return -1;
        }
        return MockPty::write(data);
    }

    void wakeupReader() override
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _wakeups += 1;
            _pendingWakeups += 1;
        }
        _wake.notify_all();
    }

    void close() override
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _closed = true;
        }
        _wake.notify_all();
        _drained.notify_all();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        auto const lock = std::lock_guard { _mutex };
        return _closed;
    }

    /// Hands @p output to the reader as the application's, waking it.
    /// @param output What the application writes.
    void sendOutput(std::string_view output)
    {
        {
            auto const lock = std::lock_guard { _mutex };
            appendStdOutBuffer(output);
        }
        _wake.notify_all();
    }

    /// From now on, a write parks.
    void stall()
    {
        auto const lock = std::lock_guard { _mutex };
        _stalled = true;
    }

    /// Lets every parked write through, and those to come.
    void drain()
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _stalled = false;
        }
        _drained.notify_all();
    }

    /// @return How often the reader was woken.
    [[nodiscard]] int wakeups() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _wakeups;
    }

    /// @return How many writes wait for a drain right now.
    [[nodiscard]] int parkedWrites() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _parkedWrites;
    }

    /// @return What was written to the application so far.
    [[nodiscard]] std::string written() const
    {
        auto const lock = std::lock_guard { _mutex };
        return stdinBuffer();
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::condition_variable _drained;
    bool _stalled = false;
    bool _closed = false;
    int _wakeups = 0;
    int _pendingWakeups = 0;
    int _parkedWrites = 0;
};

/// Opens a tab in @p host's window.
/// @param host The host to open it in.
/// @return The session of the tab's one pane.
[[nodiscard]] inline vtworkspace::SessionId newSession(SessionHost& host)
{
    host.createTab();
    return host.model().window(host.windowId())->activeTab()->rootPane()->session();
}

/// When every record promptingWire() and its siblings build was prompted, in nanoseconds since the Unix
/// epoch.
constexpr auto PromptedAt = int64_t { 1'700'000'000'000'000'000 };

/// A record off the wire, as a host sends it while the block is at its prompt: host id @p id, its command
/// line `cmd-<id>` as the application reported it, prompted at PromptedAt.
/// @param id The host's block id.
/// @return The record.
[[nodiscard]] inline proto::WireCommandBlock promptingWire(uint32_t id)
{
    auto block = proto::WireCommandBlock {};
    block.id = id;
    block.state = std::to_underlying(vtbackend::CommandBlockState::Prompting);
    block.commandLine = "cmd-" + std::to_string(id);
    block.commandLineSource = std::to_underlying(vtbackend::CommandLineSource::Reported);
    block.present = proto::CommandBlockHasPromptStart;
    block.promptStartedAt = PromptedAt;
    return block;
}

/// promptingWire() once the command started (OSC 133;C), a second after its prompt.
/// @param id The host's block id.
/// @return The record.
[[nodiscard]] inline proto::WireCommandBlock runningWire(uint32_t id)
{
    auto block = promptingWire(id);
    block.state = std::to_underlying(vtbackend::CommandBlockState::Running);
    block.present = static_cast<uint8_t>(block.present | proto::CommandBlockHasCommandStart);
    block.commandStartedAt = PromptedAt + 1'000'000'000;
    return block;
}

/// runningWire() once the command ended as OSC 133;D reported it, two seconds after it started.
/// @param id The host's block id.
/// @param exitCode The exit code it ended with.
/// @return The record.
[[nodiscard]] inline proto::WireCommandBlock finishedWire(uint32_t id, int exitCode = 0)
{
    auto block = runningWire(id);
    block.state = std::to_underlying(vtbackend::CommandBlockState::Finished);
    block.end = std::to_underlying(vtbackend::CommandBlockEnd::Reported);
    block.present = proto::CommandBlockHasPromptStart | proto::CommandBlockHasCommandStart
                    | proto::CommandBlockHasExitCode | proto::CommandBlockHasDuration;
    block.exitCode = exitCode;
    block.duration = 2'000'000'000;
    return block;
}

} // namespace vthost::testing
