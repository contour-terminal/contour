// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessRunner.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <utility>

// Windows runs no process through this runner yet: start() answers `Unsupported`, and pump() only waits to
// be woken, so that a ThreadedProcessRunner can stop.
namespace vtpty
{

namespace
{
    /// Whether a pump() was woken since it last waited.
    enum class Woken : uint8_t
    {
        No,
        Yes,
    };
} // namespace

struct PipedProcessRunner::State
{
    std::mutex mutex;
    std::condition_variable woken;
    Woken wake = Woken::No;
};

std::expected<std::unique_ptr<PipedProcessRunner>, ProcessRunnerError> PipedProcessRunner::create(
    core::platform::IClock& /*clock*/, ProcessRunnerTuning /*tuning*/)
{
    return std::make_unique<PipedProcessRunner>(Key {}, std::make_unique<State>());
}

PipedProcessRunner::PipedProcessRunner(Key /*key*/, std::unique_ptr<State> state) noexcept:
    _state { std::move(state) }
{
}

PipedProcessRunner::~PipedProcessRunner() = default;

std::expected<ProcessToken, ProcessStartError> PipedProcessRunner::start(ProcessSpec /*spec*/,
                                                                         CompletionHandler /*done*/)
{
    return std::unexpected(ProcessStartError::Unsupported);
}

void PipedProcessRunner::cancel(ProcessToken /*token*/)
{
}

void PipedProcessRunner::pump(std::chrono::milliseconds maxWait)
{
    auto lock = std::unique_lock { _state->mutex };
    _state->woken.wait_for(lock, maxWait, [this] { return _state->wake == Woken::Yes; });
    _state->wake = Woken::No;
}

void PipedProcessRunner::wake() noexcept
{
    {
        auto const lock = std::scoped_lock { _state->mutex };
        _state->wake = Woken::Yes;
    }
    _state->woken.notify_all();
}

std::size_t PipedProcessRunner::running() const noexcept
{
    return 0;
}

} // namespace vtpty
