// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>

#include <vthost/ShutdownSignals.hpp>

namespace vthost::testing
{

/// A signal source the test fires by hand: what stops a daemon whose test failed before it could, so the
/// failure is reported rather than waited on forever. A fire before the watch starts is kept for it.
class ManualShutdownSignals final: public ShutdownSignals
{
  public:
    [[nodiscard]] std::unique_ptr<Watch> watch(std::function<void()> onSignal,
                                               std::shared_future<void> lastWrite) override
    {
        auto const lock = std::scoped_lock { _mutex };
        _onSignal = std::move(onSignal);
        _lastWrite = std::move(lastWrite);
        if (_fired)
            _onSignal();
        return std::make_unique<ManualWatch>(*this);
    }

    /// Asks the watching daemon to end, now or as soon as it watches.
    void fire()
    {
        auto const lock = std::scoped_lock { _mutex };
        _fired = true;
        if (_onSignal)
            _onSignal();
    }

    /// @return What the watching daemon fulfils once its last write is done.
    [[nodiscard]] std::shared_future<void> lastWrite()
    {
        auto const lock = std::scoped_lock { _mutex };
        return _lastWrite;
    }

  private:
    class ManualWatch final: public Watch
    {
      public:
        explicit ManualWatch(ManualShutdownSignals& source): _source { source } {}
        ~ManualWatch() override
        {
            auto const lock = std::scoped_lock { _source._mutex };
            _source._onSignal = nullptr;
        }

      private:
        ManualShutdownSignals& _source;
    };

    std::mutex _mutex;
    std::function<void()> _onSignal;
    std::shared_future<void> _lastWrite;
    bool _fired = false; ///< Whether fire() was called, so a watch started later fires at once.
};

} // namespace vthost::testing
