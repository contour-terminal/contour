// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The bounded tail of a child's stderr, filled by a drain thread and read by whoever owns the child.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace vthost
{

/// The last bytes a child wrote to stderr, at most a fixed count of them.
class StderrTail
{
  public:
    /// @param limit How many of the last bytes are kept.
    explicit StderrTail(std::size_t limit): _limit { limit } {}

    /// Keeps @p bytes, dropping the oldest beyond the limit.
    void append(std::string_view bytes)
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _text.append(bytes);
            if (_text.size() > _limit)
                _text.erase(0, _text.size() - _limit);
        }
    }

    /// Marks the stream drained to its end.
    void end() noexcept
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _state = DrainState::Ended;
        }
        _ended.notify_all();
    }

    /// @return The bytes kept so far.
    [[nodiscard]] std::string text() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _text;
    }

    /// Waits until the stream was drained to its end, or @p deadline passed.
    void awaitEnd(std::chrono::steady_clock::time_point deadline) const
    {
        auto lock = std::unique_lock { _mutex };
        _ended.wait_until(lock, deadline, [this]() { return _state == DrainState::Ended; });
    }

  private:
    /// Whether the drain thread still reads.
    enum class DrainState : std::uint8_t
    {
        Draining = 0,
        Ended,
    };

    std::size_t _limit;
    mutable std::mutex _mutex;
    mutable std::condition_variable _ended;
    std::string _text;
    DrainState _state = DrainState::Draining;
};

} // namespace vthost
