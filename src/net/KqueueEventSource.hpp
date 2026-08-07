// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The macOS/BSD @c EventSource: multiplexes the registered file descriptors with
/// kqueue(2) instead of poll(2).
///
/// Same contract and same observable behaviour as @c PollEventSource — a drop-in
/// alternative selected by @c makeDefaultEventSource(), differing only in cost: a
/// wait is O(ready) rather than O(registered).
///
/// Interest is **level-triggered** (no `EV_CLEAR`), matching what @c PosixSocket and
/// the accept loop assume.
///
/// Ported from the reactor in the fastcached project (Apache-2.0, same author),
/// including the `EV_RECEIPT` handling described at @c applyInterest — without it,
/// a write that parks while no read is outstanding never gets its filter armed.

#include <cstddef>
#include <cstdint>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

    #include <unordered_map>

    #include <net/EventSource.hpp>
    #include <net/platform/NativeHandle.hpp>

namespace net
{

/// An @c EventSource backed by a kqueue.
///
/// The kqueue filter set is kept in step with the @c FdRegistry: @c attach arms the
/// requested filters, @c detach drops them. Interest is fixed for a registration's
/// lifetime — @c EventLoop only ever attaches and detaches — so a wait costs nothing
/// beyond the kevent() itself. Registration failures surface as
/// @c FdToken::invalid() so the awaiting flow fails rather than parking on a filter
/// the kernel never armed.
class KqueueEventSource: public EventSource
{
  public:
    /// Creates the kqueue.
    /// @note If `kqueue()` fails, @c good() reports false and every @c attach
    ///       refuses, so @c makeDefaultEventSource() falls back to @c PollEventSource.
    KqueueEventSource() noexcept;
    ~KqueueEventSource() override;

    KqueueEventSource(KqueueEventSource const&) = delete;
    KqueueEventSource& operator=(KqueueEventSource const&) = delete;
    KqueueEventSource(KqueueEventSource&&) = delete;
    KqueueEventSource& operator=(KqueueEventSource&&) = delete;

    [[nodiscard]] WaitOutcome wait(int timeoutMs) override;

    [[nodiscard]] FdToken attach(NativeHandle fd, FdInterest interest) override;

    void detach(FdToken token) override;

    /// @return True if the kqueue was created successfully.
    [[nodiscard]] bool good() const noexcept { return _kq >= 0; }

    /// @return The number of fds currently attached.
    [[nodiscard]] std::size_t attachedCount() const noexcept { return _registry.size(); }

  private:
    /// Arms or drops the read and write filters for @p fd to match @p interest.
    ///
    /// Submits both filter changes with `EV_RECEIPT`. That flag is load-bearing:
    /// without an eventlist, `kevent()` stops at the FIRST failing change and the
    /// remaining changes are silently never applied. Dropping a filter that was
    /// never armed fails with `ENOENT`, so the routine "no read, yes write" case —
    /// a write parking while no read is outstanding — would abort on the read
    /// delete and never arm the write filter, parking that write forever.
    /// Returns `bool` rather than `std::expected` deliberately: this is a private
    /// helper whose only caller turns a failure into @c FdToken::invalid(), which
    /// is all the @c EventSource interface can express.
    /// @param fd The descriptor whose filters to arm or drop.
    /// @param interest The readiness bits to watch.
    /// @param token The registration's identity, echoed back by @c wait.
    /// @return True if every change applied (an `ENOENT` on a filter being dropped
    ///         is the normal steady state, not a failure).
    [[nodiscard]] bool applyInterest(NativeHandle fd, FdInterest interest, FdToken token) const noexcept;

    /// Drops both filters for @p fd, ignoring a filter that was not armed.
    void dropFilters(NativeHandle fd) const noexcept;

    FdRegistry _registry; ///< Watched fds, in registration order.
    int _kq = -1;         ///< The kqueue (owned).
    /// The fd each live registration names, so detach can drop the kernel
    /// registration in O(1) without scanning the registry. Interest itself is
    /// fixed at attach — EventLoop only ever attaches and detaches — so there
    /// is nothing to reconcile per wait.
    std::unordered_map<std::uint64_t, NativeHandle> _registered;
};

} // namespace net

#endif // kqueue platforms
