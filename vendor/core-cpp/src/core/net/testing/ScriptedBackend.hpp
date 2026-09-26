// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A scripted @c IoBackend for deterministic @c EventLoop unit tests.

#include <core/net/IoBackend.hpp>
#include <core/net/detail/ReadyBatch.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace core::net::testing
{

/// Identifies one registration for the script, in attach order and starting at 1.
///
/// A test cannot name a @c ReadinessHandler the loop created inside itself, so the
/// script names registrations by the order they arrived in. A falsy id is what
/// @c ScriptedBackend::lastHandlerId reports before anything has attached.
///
/// A strong struct rather than an `enum class` for the same reason @c core::net::ParkId
/// is one: an opaque, monotonically-allocated id over a wide value space, not an
/// enumeration of named cases.
struct HandlerId
{
    std::uint64_t value = 0; ///< The registration's id; 0 means none.

    /// @return True if two ids name the same registration.
    [[nodiscard]] friend constexpr bool operator==(HandlerId, HandlerId) noexcept = default;

    /// @return True if this id names a registration (non-zero).
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }

    /// @return The sentinel for no registration.
    [[nodiscard]] static constexpr HandlerId invalid() noexcept { return HandlerId { 0 }; }
};

} // namespace core::net::testing

namespace std
{

/// Hash specialization so @c HandlerId can key an unordered container. Declared HERE,
/// between the type and its first use, for the reason @c core::net::ParkId's is.
template <>
struct hash<core::net::testing::HandlerId>
{
    /// @param id The id to hash.
    /// @return The hash of its underlying value.
    [[nodiscard]] std::size_t operator()(core::net::testing::HandlerId id) const noexcept
    {
        return std::hash<std::uint64_t> {}(id.value);
    }
};

} // namespace std

namespace core::net::testing
{

/// An @c IoBackend whose waits replay a pre-written sequence.
///
/// Each `wait()` pops the next scripted step and dispatches it, recording the timeout
/// it was called with. When the script is exhausted it THROWS: an empty step would
/// let a parked flow spin the pump forever, and a thrown error surfaces the test's
/// own bug immediately instead.
///
/// It is a real backend, not a stub: registrations go through the same
/// @c detail::ReadyBatch every native backend dispatches through, so a case written
/// against it exercises the withdrawal rule
/// ([fastcached#475](https://github.com/LASTRADA-Software/fastcached/issues/475)) and
/// runs its callbacks inside the same dispatch guard. That is what makes
/// "backends dispatch, the loop resumes" checkable here rather than only on a kernel.
class ScriptedBackend: public IoBackend
{
  public:
    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Scripted; }

    [[nodiscard]] std::expected<void, NetError> attach(ReadinessHandler& handler) override
    {
        if (_refuseNextAttach)
        {
            _refuseNextAttach = false;
            return std::unexpected { makeNetError(
                NetErrorCode::SystemError, 0, "ScriptedBackend::attach: refused on request") };
        }
        if (_byHandler.contains(&handler))
            return std::unexpected { makeNetError(
                NetErrorCode::BadHandle, 0, "ScriptedBackend::attach: handler is already attached") };
        auto const id = HandlerId { ++_nextId };
        _live.emplace(id, &handler);
        _byHandler.emplace(&handler, id);
        return {};
    }

    [[nodiscard]] std::expected<void, NetError> setInterest(ReadinessHandler& handler,
                                                            Interest interest) override
    {
        auto const found = _byHandler.find(&handler);
        if (found == _byHandler.end())
            return std::unexpected { makeNetError(
                NetErrorCode::BadHandle, 0, "ScriptedBackend::setInterest: handler is not attached") };
        if (_refuseNextSetInterest)
        {
            _refuseNextSetInterest = false;
            return std::unexpected { makeNetError(
                NetErrorCode::SystemError, 0, "ScriptedBackend::setInterest: refused on request") };
        }
        _interests[found->second] = interest;
        return {};
    }

    /// Drops @p handler's registration. IDEMPOTENT, as @c IoBackend documents and
    /// every real backend behaves.
    ///
    /// Tracked as a MAP of live registrations rather than a count, because the loop
    /// genuinely detaches twice on normal paths — `notifyHandleClosing` then
    /// `unregisterFdWaiter`; `requeueForCancellation` and `wakeAllWaiters` before
    /// `await_resume`. A counter decremented per CALL therefore reported fewer
    /// registrations than were live, so a leak assertion against this backend would
    /// have passed on one that never went away.
    /// @param handler The handler to drop; unknown and repeated ones are no-ops.
    void detach(ReadinessHandler& handler) noexcept override
    {
        if (auto const found = _byHandler.find(&handler); found != _byHandler.end())
        {
            _live.erase(found->second);
            _interests.erase(found->second);
            _byHandler.erase(found);
        }
        _batch.withdraw(handler);
    }

    /// @param timeout Recorded, never slept through.
    /// @return What the next scripted step dispatched.
    /// @throws std::runtime_error when the script is exhausted while a flow is parked.
    [[nodiscard]] WaitResult wait(std::optional<platform::SteadyDuration> timeout) override
    {
        _timeouts.push_back(timeout);
        // A wake consumes no script step. `EventLoop::post` from another thread wakes
        // the backend and nothing else, so a case about cross-thread work would
        // otherwise have to script a step per post and get the count exactly right.
        if (_pendingWakes.exchange(0, std::memory_order_acq_rel) != 0)
            return WaitResult {};
        if (_script.empty())
            throw std::runtime_error("ScriptedBackend: script exhausted while a flow is still parked");

        auto const step = _script.front();
        _script.pop_front();
        if (step.id)
            if (auto const found = _live.find(step.id); found != _live.end())
                if (auto const observed = observableBy(step.id, step.observed); observed != Readiness::None)
                    _batch.add(*found->second, observed);
        return WaitResult { .dispatched = _batch.dispatch() };
    }

    void wake() noexcept override { _pendingWakes.fetch_add(1, std::memory_order_release); }

    /// @name Scripting
    /// @{

    /// Appends a step where nothing happened.
    void pushTimeout()
    {
        _script.push_back(Step { .id = HandlerId::invalid(), .observed = Readiness::None });
    }

    /// Appends a step making @p id readable.
    /// @param id A registration previously handed out by @c attach.
    void pushReadable(HandlerId id) { _script.push_back(Step { .id = id, .observed = Readiness::Readable }); }

    /// Appends a step making @p id writable.
    /// @param id A registration previously handed out by @c attach.
    void pushWritable(HandlerId id) { _script.push_back(Step { .id = id, .observed = Readiness::Writable }); }

    /// Appends a step reporting @p observed on @p id -- several conditions at once, as one kernel
    /// answer carries them (`EPOLLIN|EPOLLOUT`). What a case needs to reach the choice a backend
    /// makes between two watched directions reported in the same wait.
    /// @param id A registration previously handed out by @c attach.
    /// @param observed What the kernel would report; filtered by the registration's interest.
    void pushReadiness(HandlerId id, Readiness observed)
    {
        _script.push_back(Step { .id = id, .observed = observed });
    }

    /// Appends a step reporting a failure on @p id — the hangup or error a kernel
    /// volunteers, which reaches @c ReadinessHandler::onError or, failing that, the
    /// direction the registration watches.
    /// @param id A registration previously handed out by @c attach.
    void pushFailure(HandlerId id) { _script.push_back(Step { .id = id, .observed = Readiness::Failed }); }

    /// Makes the next @c attach refuse, so a case can drive the registration-failed
    /// path without exhausting the process's descriptors to provoke it.
    void refuseNextAttach() noexcept { _refuseNextAttach = true; }

    /// Makes the next @c setInterest refuse — the kernel refusal
    /// ([fastcached#1054](https://github.com/LASTRADA-Software/fastcached/issues/1054))
    /// that a caller must not mistake for a live registration.
    void refuseNextSetInterest() noexcept { _refuseNextSetInterest = true; }

    /// @}

    /// @name Observation
    /// @{

    /// @return The timeouts each `wait()` was called with, in order.
    [[nodiscard]] std::vector<std::optional<platform::SteadyDuration>> const& recordedTimeouts()
        const noexcept
    {
        return _timeouts;
    }

    /// @return How many times `wait()` was called.
    [[nodiscard]] std::size_t waitCount() const noexcept { return _timeouts.size(); }

    /// @return The number of registrations currently attached.
    [[nodiscard]] std::size_t attachedCount() const noexcept { return _live.size(); }

    /// @return The id most recently handed out by @c attach, or a falsy one.
    [[nodiscard]] HandlerId lastHandlerId() const noexcept { return HandlerId { _nextId }; }

    /// @param id A registration previously handed out by @c attach.
    /// @return What it is currently watched for, or @c Interest::None if it is not
    ///         attached or was never given an interest.
    [[nodiscard]] Interest interestOf(HandlerId id) const noexcept
    {
        auto const found = _interests.find(id);
        return found == _interests.end() ? Interest::None : found->second;
    }

    /// @}

  private:
    /// What a registration watching @c interestOf(id) would actually be told about
    /// @p observed, which is how a script stops being able to say something no kernel
    /// would say.
    ///
    /// A double is only useful while a case written against it would also pass on a
    /// real backend. Two rules every real backend keeps, and this one did not:
    /// `Interest::None` is silent (poll clears the pollfd, epoll drops the
    /// registration, kqueue deletes both filters), and a
    /// direction that was never asked for is never reported. A muted or wrong-direction
    /// step therefore dispatches nothing — the wait still CONSUMES it, because a wait
    /// did happen and the kernel simply had nothing for this registration.
    ///
    /// A failure is the deliberate exception: `POLLERR`, `POLLHUP` and `POLLNVAL`
    /// arrive whether or not they were asked for, so @c Readiness::Failed survives the
    /// direction filter. It does not survive muting, because a muted registration is
    /// not in the wait set to fail.
    /// @param id The registration the step names.
    /// @param observed What the step says happened.
    /// @return The part of @p observed a real backend would report, possibly
    ///         @c Readiness::None.
    [[nodiscard]] Readiness observableBy(HandlerId id, Readiness observed) const noexcept
    {
        auto const interest = interestOf(id);
        if (interest == Interest::None)
            return Readiness::None;
        auto kept = hasReadiness(observed, Readiness::Failed) ? Readiness::Failed : Readiness::None;
        if (hasInterest(interest, Interest::Read) && hasReadiness(observed, Readiness::Readable))
            kept = kept | Readiness::Readable;
        if (hasInterest(interest, Interest::Write) && hasReadiness(observed, Readiness::Writable))
            kept = kept | Readiness::Writable;
        return kept;
    }

    /// One scripted wait: which registration became ready, and how.
    struct Step
    {
        HandlerId id {}; ///< A falsy id means "nothing happened this wait".
        Readiness observed = Readiness::None;
    };

    std::deque<Step> _script;
    std::vector<std::optional<platform::SteadyDuration>> _timeouts;
    std::unordered_map<HandlerId, ReadinessHandler*> _live;
    std::unordered_map<ReadinessHandler const*, HandlerId> _byHandler;
    std::unordered_map<HandlerId, Interest> _interests;
    detail::ReadyBatch _batch;
    std::uint64_t _nextId = 0;            ///< Source of synthetic, never-zero ids.
    std::atomic<int> _pendingWakes { 0 }; ///< `wake()` is the one member another thread may call.
    bool _refuseNextAttach = false;
    bool _refuseNextSetInterest = false;
};

} // namespace core::net::testing
