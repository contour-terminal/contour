// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessRunner.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <system_error>
#include <utility>

namespace vtpty
{

namespace
{
    /// How long the pumping thread waits when nothing falls due: start(), cancel() and the destructor wake
    /// it.
    constexpr auto IdleWait = std::chrono::milliseconds { std::chrono::hours { 1 } };
} // namespace

std::expected<std::unique_ptr<ThreadedProcessRunner>, ProcessRunnerError> ThreadedProcessRunner::create(
    core::platform::IClock& clock, ProcessRunnerTuning tuning)
{
    auto runner = PipedProcessRunner::create(clock, tuning);
    if (!runner)
        return std::unexpected(runner.error());
    try
    {
        return std::make_unique<ThreadedProcessRunner>(Key {}, std::move(*runner));
    }
    catch (std::system_error const&)
    {
        return std::unexpected(ProcessRunnerError::ThreadUnavailable);
    }
}

ThreadedProcessRunner::ThreadedProcessRunner(Key /*key*/, std::unique_ptr<PipedProcessRunner> runner):
    _runner { std::move(runner) }, _thread { [this] {
        while (_pumping.load(std::memory_order_acquire) == Pumping::Running)
            _runner->pump(IdleWait);
    } }
{
}

ThreadedProcessRunner::~ThreadedProcessRunner()
{
    _pumping.store(Pumping::Stopping, std::memory_order_release);
    _runner->wake();
    _thread.join();
}

std::expected<ProcessToken, ProcessStartError> ThreadedProcessRunner::start(ProcessSpec spec,
                                                                            CompletionHandler done)
{
    return _runner->start(std::move(spec), std::move(done));
}

void ThreadedProcessRunner::cancel(ProcessToken token)
{
    _runner->cancel(token);
}

} // namespace vtpty
