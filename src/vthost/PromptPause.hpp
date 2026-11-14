// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The time a user spends answering prompts during an attach, which the attach's clock does not count: an
/// outer deadline cannot see a question put to the user, as AttachBudget::promptsOpen's comment says of ssh.
/// Used for the passphrase of an `identity_file`, asked while the handshake runs.

#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <utility>

#include <vthost/Askpass.hpp>

namespace vthost
{

/// How long prompts have been open in all: the ones answered, and those of every session open now. Any
/// thread.
///
/// The total only grows, so the prompt time of one attempt is what it grew by since the attempt began
/// (AttachBudget::startingNow()).
class PromptPause
{
  public:
    /// Names one open session, for close().
    using SessionId = std::uint64_t;

    /// Notes that a prompt session began. Sessions may overlap: each is counted until it closes.
    /// @param openTime The session's total prompt time so far (IAskpassSession::promptTime()); read until
    ///        close(), so it must stay callable until then.
    /// @return What names the session to close().
    [[nodiscard]] SessionId open(std::function<std::chrono::milliseconds()> openTime)
    {
        auto const lock = std::scoped_lock { _mutex };
        auto const id = _nextId++;
        _open.emplace(id, std::move(openTime));
        return id;
    }

    /// Notes that a session ended.
    /// @param session What open() named it.
    /// @param finalTime Its total prompt time.
    void close(SessionId session, std::chrono::milliseconds finalTime)
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_open.erase(session) != 0)
            _closed += finalTime;
    }

    /// @return How many sessions are open.
    [[nodiscard]] std::size_t openSessions() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _open.size();
    }

    /// @return The prompt time of the sessions that ended, and of every one that is open.
    [[nodiscard]] std::chrono::milliseconds total() const
    {
        auto const lock = std::scoped_lock { _mutex };
        auto sum = _closed;
        for (auto const& openTime: _open | std::views::values)
            sum += openTime();
        return sum;
    }

  private:
    mutable std::mutex _mutex;
    std::chrono::milliseconds _closed = std::chrono::milliseconds::zero();
    SessionId _nextId = 0;
    std::map<SessionId, std::function<std::chrono::milliseconds()>> _open;
};

/// An askpass broker whose sessions report their prompt time to a PromptPause.
class PausingAskpassBroker final: public IAskpassBroker
{
  public:
    /// @param inner Opens the sessions.
    /// @param pause Told how long their prompts are open, and how long the wait for @p inner to serve takes;
    ///        shared with whoever reads it.
    /// @param clock Measures that wait; borrowed.
    PausingAskpassBroker(std::shared_ptr<IAskpassBroker> inner,
                         std::shared_ptr<PromptPause> pause,
                         core::platform::IClock const& clock = core::platform::defaultSteadyClock()):
        _inner { std::move(inner) }, _pause { std::move(pause) }, _clock { clock }
    {
    }

    /// @copydoc IAskpassBroker::whenServing
    /// The wait counts as prompt time: it waits for the application that asks the prompts, which the GUI may
    /// still be making, and the attach's deadline must not be charged for it.
    [[nodiscard]] core::async::Task<std::expected<void, std::string>> whenServing() override
    {
        auto const waiting = Waiting { *_pause, _clock };
        co_return co_await _inner->whenServing();
    }

    /// @copydoc IAskpassBroker::open
    [[nodiscard]] std::expected<std::unique_ptr<IAskpassSession>, std::string> open(
        SshTarget const& target) override
    {
        return _inner->open(target).transform(
            [this](std::unique_ptr<IAskpassSession> session) -> std::unique_ptr<IAskpassSession> {
                return std::make_unique<Session>(std::move(session), _pause);
            });
    }

  private:
    /// Counts the time from its construction to its destruction as one prompt session of a PromptPause.
    class Waiting
    {
      public:
        Waiting(PromptPause& pause, core::platform::IClock const& clock):
            _pause { pause },
            _clock { clock },
            _began { clock.now() },
            _id { _pause.open([this] { return elapsed(); }) }
        {
        }
        ~Waiting() { _pause.close(_id, elapsed()); }
        Waiting(Waiting const&) = delete;
        Waiting& operator=(Waiting const&) = delete;
        Waiting(Waiting&&) = delete;
        Waiting& operator=(Waiting&&) = delete;

      private:
        [[nodiscard]] std::chrono::milliseconds elapsed() const
        {
            return std::chrono::ceil<std::chrono::milliseconds>(_clock.now() - _began);
        }

        PromptPause& _pause;
        core::platform::IClock const& _clock;
        core::platform::SteadyTimePoint _began;
        PromptPause::SessionId _id;
    };

    class Session final: public IAskpassSession
    {
      public:
        Session(std::unique_ptr<IAskpassSession> inner, std::shared_ptr<PromptPause> pause):
            _inner { std::move(inner) },
            _pause { std::move(pause) },
            _id { _pause->open([inner = _inner.get()] { return inner->promptTime(); }) }
        {
        }
        ~Session() override { _pause->close(_id, _inner->promptTime()); }

        Session(Session const&) = delete;
        Session& operator=(Session const&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

        [[nodiscard]] SpawnEnvironment environment() const override { return _inner->environment(); }
        [[nodiscard]] bool wasCancelled() const noexcept override { return _inner->wasCancelled(); }
        [[nodiscard]] std::chrono::milliseconds promptTime() const noexcept override
        {
            return _inner->promptTime();
        }
        void onCancelled(std::function<void()> wake) override { _inner->onCancelled(std::move(wake)); }

      private:
        std::unique_ptr<IAskpassSession> _inner;
        std::shared_ptr<PromptPause> _pause;
        PromptPause::SessionId _id;
    };

    std::shared_ptr<IAskpassBroker> _inner;
    std::shared_ptr<PromptPause> _pause;
    core::platform::IClock const& _clock;
};

} // namespace vthost
