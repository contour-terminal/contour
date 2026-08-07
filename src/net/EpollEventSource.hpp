// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Linux @c EventSource: multiplexes the registered file descriptors with
/// epoll(7) instead of poll(2).
///
/// Same contract and same observable behaviour as @c PollEventSource — it is a
/// drop-in alternative, selected by @c makeDefaultEventSource(). The difference is
/// cost: poll(2) hands the kernel the whole watch set on every call and scans it
/// again on return, so a wait is O(registered); epoll keeps the set in the kernel
/// and reports only what became ready, so a wait is O(ready). That is irrelevant
/// for a terminal watching a handful of descriptors and decisive for a server
/// holding thousands of idle connections.
///
/// Interest is **level-triggered** (no `EPOLLET`), matching what @c PosixSocket and
/// the accept loop assume: a descriptor that stays ready is reported again on the
/// next wait, so a partial read does not have to drain to `EAGAIN` to stay live.
///
/// Ported from the reactor in the fastcached project (Apache-2.0, same author).

#include <cstddef>
#include <cstdint>

#ifdef __linux__

    #include <unordered_map>

    #include <net/EventSource.hpp>
    #include <net/platform/NativeHandle.hpp>

namespace net
{

/// An @c EventSource backed by an epoll instance.
///
/// The epoll set is kept in step with the @c FdRegistry: @c attach adds the
/// descriptor with its interest, @c detach removes it. Interest is fixed for a
/// registration's lifetime — @c EventLoop only ever attaches and detaches, and
/// @c FdRegistry exposes no way to change it — so a wait costs nothing beyond the
/// epoll_wait itself. Registration failures surface as @c FdToken::invalid() so the
/// awaiting flow fails rather than parking on an interest the kernel never accepted.
class EpollEventSource: public EventSource
{
  public:
    /// Creates the epoll instance.
    /// @note Construction cannot fail usefully — if `epoll_create1` fails, @c good()
    ///       reports false and every @c attach refuses, so a caller can fall back to
    ///       @c PollEventSource. Use @c makeDefaultEventSource() to get that for free.
    EpollEventSource() noexcept;
    ~EpollEventSource() override;

    EpollEventSource(EpollEventSource const&) = delete;
    EpollEventSource& operator=(EpollEventSource const&) = delete;
    EpollEventSource(EpollEventSource&&) = delete;
    EpollEventSource& operator=(EpollEventSource&&) = delete;

    [[nodiscard]] WaitOutcome wait(int timeoutMs) override;

    [[nodiscard]] FdToken attach(NativeHandle fd, FdInterest interest) override;

    void detach(FdToken token) override;

    /// @return True if the epoll instance was created successfully.
    [[nodiscard]] bool good() const noexcept { return _epollFd >= 0; }

    /// @return The number of fds currently attached.
    [[nodiscard]] std::size_t attachedCount() const noexcept { return _registry.size(); }

  private:
    /// Registers @p fd with @p interest.
    ///
    /// Returns `bool` rather than `std::expected` deliberately: this is a private
    /// helper whose only caller turns a failure into @c FdToken::invalid(), which
    /// is all the @c EventSource interface can express. There is nowhere for a
    /// richer error to go, so carrying one would be discarded at the next frame.
    /// @param fd The descriptor to register.
    /// @param interest The readiness bits to watch.
    /// @param token The registration's identity, echoed back by @c wait.
    /// @return True if the epoll_ctl call succeeded.
    [[nodiscard]] bool applyInterest(NativeHandle fd, FdInterest interest, FdToken token) const noexcept;

    /// The descriptor a registration is watched through, and whether this source
    /// created it and must therefore close it.
    struct Watched
    {
        NativeHandle fd = InvalidHandle; ///< The descriptor handed to epoll_ctl.
        bool owned = false;              ///< True if this is our dup(), false if the caller's fd.
    };

    FdRegistry _registry; ///< Watched fds, in registration order.
    int _epollFd = -1;    ///< The epoll instance (owned).
    /// The descriptor each live registration is watched through, so detach can drop
    /// the kernel registration in O(1) without scanning the registry. Interest itself
    /// is fixed at attach — EventLoop only ever attaches and detaches — so there is
    /// nothing to reconcile per wait.
    ///
    /// Normally this is the CALLER'S descriptor. A `dup()` would share the underlying
    /// open file description, so the caller closing its copy would not release it —
    /// no FIN would reach the peer, whose read would block forever rather than
    /// observe EOF (poll(2), which holds no descriptor of its own, has no such
    /// effect). A duplicate is therefore made only for a genuine second registration
    /// of one descriptor, which an epoll set rejects with `EEXIST` while poll(2)
    /// simply takes two entries; the registry permits it, so this backend must too.
    std::unordered_map<std::uint64_t, Watched> _registered;
};

} // namespace net

#endif // __linux__
