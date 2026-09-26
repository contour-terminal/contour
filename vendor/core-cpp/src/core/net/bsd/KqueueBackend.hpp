// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The macOS/BSD @c IoBackend: multiplexes the registered descriptors with kqueue(2)
/// instead of poll(2).
///
/// Same contract and the same observable behaviour as @c PollBackend — that is what
/// `BackendParity_test` is for — differing only in cost: a wait is O(ready) rather
/// than O(registered).
///
/// Interest is level-triggered (no `EV_CLEAR`), matching what the sockets and the
/// accept loop assume.
///
/// Ported from the reactor in the fastcached project (Apache-2.0, same author),
/// including the `EV_RECEIPT` handling described at @c arm — without it, a write that
/// parks while no read is outstanding never gets its filter armed. See
/// `.agent/reference/provenance.md`.

#include <core/net/IoBackend.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/detail/WakeupChannel.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <unordered_map>

namespace core::net
{

/// An @c IoBackend backed by a kqueue.
class KqueueBackend final: public IoBackend
{
  public:
    /// Creates the kqueue and the wakeup channel.
    /// @note If `kqueue()` fails — descriptor exhaustion — @c good() reports false and
    ///       every @c attach refuses, so @c makeBackend answers null and
    ///       @c makeDefaultBackend falls back to @c PollBackend.
    /// @throws std::runtime_error if the wakeup channel cannot be created or registered.
    KqueueBackend();
    ~KqueueBackend() override;

    KqueueBackend(KqueueBackend const&) = delete;
    KqueueBackend& operator=(KqueueBackend const&) = delete;
    KqueueBackend(KqueueBackend&&) = delete;
    KqueueBackend& operator=(KqueueBackend&&) = delete;

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Kqueue; }

    /// Validates @p handler against this backend. **This registers NOTHING with the
    /// kernel, and that is correct rather than missing**: kqueue has no "add this
    /// descriptor with no filters" operation, so interest arrives per filter through
    /// `EV_ADD`, which is what @c setInterest does. @c EpollBackend::attach genuinely
    /// calls `epoll_ctl(EPOLL_CTL_ADD)` because epoll HAS that operation; the shapes
    /// differ because the two kernels differ, not because one of them forgot.
    /// ([fastcached#1057](https://github.com/LASTRADA-Software/fastcached/issues/1057)
    /// was filed against the fastcached body this is ported from, as a missing kernel
    /// call with epoll named as the control.)
    /// @param handler The handler to register.
    /// @return Whether that handler and this backend are usable together; never
    ///         whether the kernel knows about the descriptor, which only
    ///         @c setInterest can say.
    [[nodiscard]] std::expected<void, NetError> attach(ReadinessHandler& handler) override;

    [[nodiscard]] std::expected<void, NetError> setInterest(ReadinessHandler& handler,
                                                            Interest interest) override;

    void detach(ReadinessHandler& handler) noexcept override;

    [[nodiscard]] WaitResult wait(std::optional<platform::SteadyDuration> timeout) override;

    void wake() noexcept override { _wakeup.signal(); }

    /// @return True if the kqueue was created successfully.
    [[nodiscard]] bool good() const noexcept { return _kq >= 0; }

  private:
    /// One registered handler: what it is watched for, and through which descriptor.
    struct Registration
    {
        Interest interest = Interest::None; ///< What it is watched for; None arms no filter.

        /// The descriptor the filters are armed on.
        ///
        /// Normally the CALLER'S. A `dup()` would share the underlying open file
        /// description, so the caller closing its copy would not release it — no FIN
        /// would reach the peer, whose read would block forever rather than observe
        /// EOF. A duplicate is made only for a genuine SECOND registration of one
        /// descriptor: a kqueue filter is keyed by (descriptor, filter), so a second
        /// registration would REPLACE the first's filters rather than stand beside
        /// them, and dropping either would tear down both.
        platform::NativeHandle watched = platform::InvalidHandle;

        bool owned = false; ///< True if @c watched is this backend's dup(), false if the caller's.
        bool armed = false; ///< True while at least one filter is armed.
    };

    /// Arms the read and write filters on @p registration to match @p interest.
    ///
    /// Submits both filter changes with `EV_RECEIPT`. That flag is load-bearing:
    /// without an eventlist, `kevent()` stops at the FIRST failing change and the rest
    /// are silently never applied. Dropping a filter that was never armed fails with
    /// `ENOENT`, so the routine "no read, yes write" case — a write parking while no
    /// read is outstanding — would abort on the read delete and never arm the write
    /// filter, parking that write forever holding its unsent tail.
    /// @param handler The attached handler, whose address the events carry back.
    /// @param registration Its record, updated in place on success.
    /// @param interest The readiness to arm (never @c Interest::None).
    /// @return Nothing, or the errno the kernel refused with.
    [[nodiscard]] std::expected<void, NetError> arm(ReadinessHandler& handler,
                                                    Registration& registration,
                                                    Interest interest);

    /// Drops both filters for @p registration, ignoring one that was not armed. Keeps
    /// any private duplicate, so re-arming the same handler costs no second descriptor.
    void disarm(Registration& registration) const noexcept;

    /// The wakeup channel is declared BEFORE the kernel descriptor, and the order is
    /// load-bearing rather than tidy: @c detail::WakeupChannel's constructor throws
    /// under descriptor exhaustion, a constructor that throws from its mem-init list
    /// does not run the class destructor, and an `int` has none of its own. Built the
    /// other way round, the kqueue was created first and leaked on exactly the pressure
    /// that makes a descriptor worth having -- which also made
    /// @c makeDefaultBackend()'s documented fallback to @c PollBackend less likely to
    /// succeed. Declared first, it is constructed first and `kqueue()` is never reached.
    detail::WakeupChannel _wakeup; ///< How another thread breaks a wait in flight.
    int _kq = -1;                  ///< The kqueue (owned).
    detail::ReadyBatch _batch;     ///< What this wait found ready, and what `detach` withdraws from.

    /// The live registrations, keyed by the handler's address — which is the
    /// registration's identity, since two registrations may share a descriptor.
    std::unordered_map<ReadinessHandler const*, Registration> _registrations;
};

} // namespace core::net
