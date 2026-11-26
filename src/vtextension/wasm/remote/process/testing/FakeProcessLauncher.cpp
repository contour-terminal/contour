// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/testing/FakeProcessLauncher.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace vtextension::wasm::remote::process::testing
{

namespace
{
    /// One process launched.
    struct LaunchRecord
    {
        LaunchSpec spec;
        std::thread::id thread;
        std::optional<ExitStatus> exit; ///< How it ended; nothing while it runs.
    };
} // namespace

struct FakeProcessLauncher::State
{
    mutable std::mutex mutex;
    std::condition_variable changed; ///< Notified on every exit and every wake.
    std::vector<LaunchRecord> launches;
    std::vector<std::size_t> kills;
    std::optional<core::SteadyTimePoint> lastDeadline;
    std::optional<LaunchError> waitFailure;  ///< What every wait() fails with; nothing while they work.
    std::optional<LaunchError> watchFailure; ///< What every watch() fails with; nothing while they work.
    /// What every handleTarget() fails with; nothing while they work.
    std::optional<LaunchError> targetFailure;
    std::vector<std::thread::id> targetThreads; ///< The thread each handleTarget() ran on, in call order.
    std::set<std::size_t> survivors;            ///< The processes kill() does not end.
    std::map<std::size_t, LaunchError>
        reapFailures; ///< What each process's tryReap() fails with, if it does.
};

namespace
{
    using State = FakeProcessLauncher::State;

    class FakeProcessHandle final: public IProcessHandle
    {
      public:
        FakeProcessHandle(std::shared_ptr<State> state, std::size_t index) noexcept:
            _state { std::move(state) }, _index { index }
        {
        }

        [[nodiscard]] ProcessId id() const noexcept override
        {
            return FirstFakeProcessId + static_cast<ProcessId>(_index);
        }

        void kill() noexcept override
        {
            auto const lock = std::scoped_lock { _state->mutex };
            _state->kills.push_back(_index);
            auto& record = _state->launches[_index];
            if (!record.exit && !_state->survivors.contains(_index))
            {
                record.exit = FakeKilledStatus;
                _state->changed.notify_all();
            }
        }

        [[nodiscard]] std::expected<std::optional<ExitStatus>, LaunchError> tryReap() noexcept override
        {
            auto const lock = std::scoped_lock { _state->mutex };
            if (auto const failure = _state->reapFailures.find(_index); failure != _state->reapFailures.end())
                return std::unexpected(failure->second);
            return _state->launches[_index].exit;
        }

        [[nodiscard]] Waitable waitable() const noexcept override { return Waitable {}; }

        [[nodiscard]] std::expected<transport::HandleTarget, LaunchError> handleTarget() override
        {
            auto const lock = std::scoped_lock { _state->mutex };
            _state->targetThreads.push_back(std::this_thread::get_id());
            if (_state->targetFailure)
                return std::unexpected(*_state->targetFailure);
            return transport::HandleTarget::currentProcess();
        }

        /// @return Which process this is, counted from 0 in launch order.
        [[nodiscard]] std::size_t index() const noexcept { return _index; }

      private:
        std::shared_ptr<State> _state;
        std::size_t _index;
    };

    class FakeExitWaitSet final: public IExitWaitSet
    {
      public:
        explicit FakeExitWaitSet(std::shared_ptr<State> state) noexcept: _state { std::move(state) } {}

        [[nodiscard]] std::expected<void, LaunchError> watch(uint64_t key,
                                                             IProcessHandle const& process) override
        {
            auto const* const fake = dynamic_cast<FakeProcessHandle const*>(&process);
            if (fake == nullptr)
                return std::unexpected(LaunchError::Unsupported);
            auto const lock = std::scoped_lock { _state->mutex };
            if (_state->watchFailure)
                return std::unexpected(*_state->watchFailure);
            _watched.insert_or_assign(key, fake->index());
            return {};
        }

        void unwatch(uint64_t key) noexcept override
        {
            auto const lock = std::scoped_lock { _state->mutex };
            _watched.erase(key);
        }

        [[nodiscard]] std::expected<std::vector<uint64_t>, LaunchError> wait(
            std::optional<core::SteadyTimePoint> deadline) override
        {
            auto lock = std::unique_lock { _state->mutex };
            _state->lastDeadline = deadline;
            auto ended = std::vector<uint64_t> {};
            _state->changed.wait(lock, [&] {
                ended = endedKeys();
                return _isWoken || !ended.empty() || _state->waitFailure;
            });
            if (_state->waitFailure)
                return std::unexpected(*_state->waitFailure);
            _isWoken = false;
            return ended;
        }

        void wake() noexcept override
        {
            auto const lock = std::scoped_lock { _state->mutex };
            _isWoken = true;
            _state->changed.notify_all();
        }

      private:
        /// @return The keys whose process ended. The state's mutex is held.
        [[nodiscard]] std::vector<uint64_t> endedKeys() const
        {
            auto keys = std::vector<uint64_t> {};
            for (auto const& [key, index]: _watched)
                if (_state->launches[index].exit)
                    keys.push_back(key);
            return keys;
        }

        std::shared_ptr<State> _state;
        std::map<uint64_t, std::size_t> _watched; ///< Guarded by the state's mutex.
        bool _isWoken = false;                    ///< Guarded by the state's mutex.
    };
} // namespace

FakeProcessLauncher::FakeProcessLauncher(): FakeProcessLauncher { LaunchHook {} }
{
}

FakeProcessLauncher::FakeProcessLauncher(LaunchHook onLaunch):
    _state { std::make_shared<State>() }, _onLaunch { std::move(onLaunch) }
{
}

FakeProcessLauncher::~FakeProcessLauncher() = default;

std::expected<std::unique_ptr<IProcessHandle>, LaunchError> FakeProcessLauncher::launch(
    LaunchSpec const& spec)
{
    auto const index = [&] {
        auto const lock = std::scoped_lock { _state->mutex };
        _state->launches.push_back(
            LaunchRecord { .spec = spec, .thread = std::this_thread::get_id(), .exit = std::nullopt });
        return _state->launches.size() - 1;
    }();
    if (_onLaunch)
        _onLaunch(index, spec);
    return std::make_unique<FakeProcessHandle>(_state, index);
}

std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> FakeProcessLauncher::makeWaitSet(
    [[maybe_unused]] core::IClock& clock)
{
    return std::make_unique<FakeExitWaitSet>(_state);
}

void FakeProcessLauncher::failReap(std::size_t index, LaunchError error)
{
    auto const lock = std::scoped_lock { _state->mutex };
    _state->reapFailures.insert_or_assign(index, error);
    _state->changed.notify_all();
}

void FakeProcessLauncher::exit(std::size_t index, ExitStatus status)
{
    auto const lock = std::scoped_lock { _state->mutex };
    _state->launches.at(index).exit = status;
    _state->changed.notify_all();
}

void FakeProcessLauncher::survivesKill(std::size_t index)
{
    auto const lock = std::scoped_lock { _state->mutex };
    _state->survivors.insert(index);
}

void FakeProcessLauncher::failWatches(LaunchError error)
{
    auto const lock = std::scoped_lock { _state->mutex };
    _state->watchFailure = error;
}

void FakeProcessLauncher::failHandleTargets(LaunchError error)
{
    auto const lock = std::scoped_lock { _state->mutex };
    _state->targetFailure = error;
}

std::vector<std::thread::id> FakeProcessLauncher::handleTargetThreads() const
{
    auto const lock = std::scoped_lock { _state->mutex };
    return _state->targetThreads;
}

void FakeProcessLauncher::failWaits(LaunchError error)
{
    auto const lock = std::scoped_lock { _state->mutex };
    _state->waitFailure = error;
    _state->changed.notify_all();
}

std::vector<std::size_t> FakeProcessLauncher::kills() const
{
    auto const lock = std::scoped_lock { _state->mutex };
    return _state->kills;
}

std::vector<LaunchSpec> FakeProcessLauncher::launches() const
{
    auto const lock = std::scoped_lock { _state->mutex };
    auto specs = std::vector<LaunchSpec> {};
    for (auto const& record: _state->launches)
        specs.push_back(record.spec);
    return specs;
}

std::vector<std::thread::id> FakeProcessLauncher::launchThreads() const
{
    auto const lock = std::scoped_lock { _state->mutex };
    auto threads = std::vector<std::thread::id> {};
    for (auto const& record: _state->launches)
        threads.push_back(record.thread);
    return threads;
}

std::optional<core::SteadyTimePoint> FakeProcessLauncher::lastDeadline() const
{
    auto const lock = std::scoped_lock { _state->mutex };
    return _state->lastDeadline;
}

} // namespace vtextension::wasm::remote::process::testing
