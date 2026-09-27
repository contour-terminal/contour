// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The ready batch every @c IoBackend dispatches through, and the guard that makes
/// "backends dispatch, the loop resumes" checkable rather than merely stated.
///
/// A backend's wait produces a batch the kernel already wrote — an `epoll_event`
/// array, a `kevent` list, a scan of `pollfd` revents — and then walks it, invoking
/// one callback per entry. Two things must hold during that walk, and both were
/// defects before they were rules:
///
/// - **An entry withdrawn mid-walk is not dispatched.** A callback may detach another
///   registration and free its owner; the kernel's array still holds that
///   registration's entry, and the walk would read it (fastcached#475). The batch is
///   therefore the single place `detach` scrubs, so the four backends share one
///   implementation of it instead of four chances to get it wrong.
/// - **Nothing resumes a coroutine.** @c ReadinessDispatchGuard publishes that a
///   dispatch is in flight on this thread, and the event loop asserts it is not while
///   it resumes — so a backend that ever resumed inline fails an assertion with a
///   stack, instead of corrupting the walk it is inside of.

#include <core/net/IoBackend.hpp>

#include <cassert>
#include <cstddef>
#include <ranges>
#include <vector>

namespace core::net::detail
{

/// How deeply readiness dispatch is nested on this thread; 0 outside any dispatch.
///
/// A counter rather than a flag, and thread-local rather than global, because a
/// process may run one loop per thread and each dispatches independently. Exposed as
/// a reference so @c ReadinessDispatchGuard is the only writer worth having; callers
/// ask @c readinessDispatchInFlight().
/// @return The current thread's nesting counter.
[[nodiscard]] inline std::size_t& readinessDispatchDepth() noexcept
{
    static thread_local auto depth = std::size_t { 0 };
    return depth;
}

/// @return True while this thread is inside a backend's readiness dispatch.
///
/// The event loop asserts this is false wherever it resumes a coroutine, which is
/// what turns Rule 1 from a convention into a diagnosed precondition: a backend that
/// resumed inline would be resuming a frame that can free the object whose batch
/// entry the dispatch has not reached yet.
[[nodiscard]] inline bool readinessDispatchInFlight() noexcept
{
    return readinessDispatchDepth() != 0;
}

/// Publishes, for the length of a scope, that this thread is dispatching readiness.
class ReadinessDispatchGuard
{
  public:
    ReadinessDispatchGuard() noexcept { ++readinessDispatchDepth(); }
    ~ReadinessDispatchGuard() { --readinessDispatchDepth(); }

    ReadinessDispatchGuard(ReadinessDispatchGuard const&) = delete;
    ReadinessDispatchGuard& operator=(ReadinessDispatchGuard const&) = delete;
    ReadinessDispatchGuard(ReadinessDispatchGuard&&) = delete;
    ReadinessDispatchGuard& operator=(ReadinessDispatchGuard&&) = delete;
};

/// The registrations one wait found ready, and the dispatch over them.
///
/// Every backend owns one and uses it the same way: collect while reading the
/// kernel's answer, then @c dispatch once. @c withdraw is what @c IoBackend::detach
/// calls, and it is why the two steps are separate — between them, and during the
/// dispatch itself, a handler may be detached and freed.
class ReadyBatch
{
  public:
    /// Records that @p handler was reported with @p observed.
    ///
    /// A handler reported twice in one batch gets ONE entry, with the two readiness
    /// masks merged. kqueue is why: it answers per (descriptor, filter), so a
    /// registration watching both directions arrives as two events and would
    /// otherwise be dispatched twice in one wait — and the first callback may leave
    /// the object the handler is embedded in ready to be freed. Merging here rather
    /// than in each backend means "at most one callback per registration per wait"
    /// holds by construction on all of them, including the ones whose kernel never
    /// duplicates.
    ///
    /// Collection only: calling this during a dispatch would reallocate the vector
    /// the dispatch is walking, so it is a precondition that it does not happen. No
    /// backend can reach it — a callback that re-entered `wait()` would break far
    /// more than this — and the assertion says so at the one place it would show.
    /// @param handler The ready registration.
    /// @param observed What the kernel reported for it.
    void add(ReadinessHandler& handler, Readiness observed)
    {
        assert(!_dispatching && "ReadyBatch::add during dispatch: a callback re-entered wait()");
        for (auto& entry: _entries)
            if (entry.handler == &handler)
            {
                entry.observed = entry.observed | observed;
                return;
            }
        _entries.push_back(Entry { .handler = &handler, .observed = observed });
    }

    /// Invokes at most one callback per recorded entry, in the order the kernel
    /// reported them, skipping entries @c withdraw took back.
    ///
    /// The batch is emptied before this returns, so a @c withdraw arriving after the
    /// wait has finished scans nothing.
    /// @return How many callbacks ran.
    [[nodiscard]] std::size_t dispatch() noexcept
    {
        auto const guard = ReadinessDispatchGuard {};
        _dispatching = true;
        auto ran = std::size_t { 0 };
        // Indexed rather than iterated: `withdraw` runs from inside a callback and
        // writes into these entries. It only nulls pointers, so no iterator is
        // invalidated, but the index makes that independent of the container. The
        // bound is taken once, which is also right: `add` is a precondition violation
        // during a dispatch, so the batch cannot grow under the walk.
        for (auto const index: std::views::iota(std::size_t { 0 }, _entries.size()))
        {
            auto const entry = _entries[index];
            if (entry.handler == nullptr)
                continue; // withdrawn after the batch was collected
            if (auto* const callback = selectReadinessCallback(*entry.handler, entry.observed);
                callback != nullptr)
            {
                callback(*entry.handler);
                ++ran;
            }
        }
        _dispatching = false;
        _entries.clear();
        return ran;
    }

    /// Takes @p handler back out of the batch, dispatched or not.
    ///
    /// The whole batch is scanned, entries already dispatched included: nulling one
    /// of those is a no-op, and it keeps this free of an off-by-one against the
    /// dispatch's cursor.
    /// @param handler The handler being detached. Must still be alive, which is what
    ///        makes the comparison against a live address rather than a recycled one.
    void withdraw(ReadinessHandler const& handler) noexcept
    {
        for (auto& entry: _entries)
            if (entry.handler == &handler)
                entry.handler = nullptr;
    }

    /// Drops every entry without dispatching any. For a wait that collected a batch
    /// and then found it had nothing to do with it.
    void clear() noexcept { _entries.clear(); }

    /// @return The number of entries collected, withdrawn ones included.
    [[nodiscard]] std::size_t size() const noexcept { return _entries.size(); }

  private:
    /// One ready registration and what was seen for it.
    struct Entry
    {
        ReadinessHandler* handler = nullptr; ///< Null once withdrawn.
        Readiness observed = Readiness::None;
    };

    std::vector<Entry> _entries;
    bool _dispatching = false; ///< Guards @c add against a re-entrant collection.
};

} // namespace core::net::detail
