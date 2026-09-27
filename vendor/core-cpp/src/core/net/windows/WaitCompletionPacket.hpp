// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `NtAssociateWaitCompletionPacket` — the kernel's own bridge from a waitable HANDLE
/// to a completion port — behind a probe, because it is not a documented API.
///
/// A completion port cannot wait on a console handle, an event or a mutex; only on
/// overlapped operations. The portable bridge is a thread-pool wait whose callback
/// posts to the port, and that is what @c IocpBackend uses when this is unavailable.
/// The kernel has a better one: a *wait completion packet* is an object that posts a
/// completion to a port when a target handle signals, with no helper thread and no
/// callback at all — so guarantee G3 (helper threads only post) is satisfied by there
/// being no helper thread.
///
/// **It is an optimisation, not the path.** The three calls are exported by `ntdll` and
/// declared in no SDK header, so they are resolved with `GetProcAddress` at startup and
/// their absence is an ordinary answer, not a failure: core-cpp never links `ntdll` and
/// never assumes the symbols are there. They have existed since Windows 8 and are what
/// the .NET and Rust runtimes use for the same job, which is the reason to take them
/// when they answer and the reason not to depend on them.
///
/// Origin: the design spec, Part I §2, "IOCP readiness bridging".

#include <cstdint>

namespace core::net::detail
{

/// Whether this process can use wait completion packets.
///
/// Resolved once, on the first call, and the answer never changes for the life of the
/// process. Thread-safe: it is a function-local static.
/// @return True when all three `Nt*WaitCompletionPacket` entry points were found.
[[nodiscard]] bool waitCompletionPacketsAvailable() noexcept;

/// One reusable kernel object that posts a completion packet when a handle signals.
///
/// Reusable is the point: the same packet is associated, delivered, and associated
/// again for every turn a registration is watched, so watching a console handle for an
/// hour costs one kernel object rather than one per turn.
class WaitCompletionPacket
{
  public:
    /// Creates the packet object, or leaves this invalid where the entry points are
    /// missing or the kernel refused. Never throws: an unavailable optimisation is an
    /// ordinary answer, and @c IocpBackend falls back to its thread-pool wait.
    WaitCompletionPacket() noexcept;
    ~WaitCompletionPacket();

    WaitCompletionPacket(WaitCompletionPacket const&) = delete;
    WaitCompletionPacket& operator=(WaitCompletionPacket const&) = delete;
    WaitCompletionPacket(WaitCompletionPacket&&) = delete;
    WaitCompletionPacket& operator=(WaitCompletionPacket&&) = delete;

    /// @return True when the object exists and may be associated.
    [[nodiscard]] bool valid() const noexcept { return _handle != nullptr; }

    /// Arms this packet: when @p target signals, one completion is posted to @p port.
    ///
    /// One association at a time, and it is consumed by delivery — the packet must be
    /// associated again for the next turn, which is exactly the level-triggered re-arm
    /// @c IocpBackend performs at the top of every wait.
    ///
    /// A target that is ALREADY signalled queues the packet immediately, which is what
    /// makes this level-triggered rather than edge-triggered, and is why a readiness
    /// raised before the watch began is not lost.
    /// @param port The completion port handle to post to.
    /// @param target The waitable handle to watch (not owned).
    /// @param key The completion key the packet carries (`lpCompletionKey`).
    /// @param context The pointer the packet carries as `lpOverlapped`.
    /// @return True when the association was made.
    [[nodiscard]] bool associate(void* port, void* target, std::uintptr_t key, void* context) noexcept;

    /// Takes the association back, and the queued packet with it.
    ///
    /// **The return value is an ownership answer, not a status.** True means no packet
    /// will be delivered for this association AND none is sitting in the port, so the
    /// caller — and only the caller — still holds whatever share the arm took. False
    /// means a packet is, or was, in flight and the dequeue owns that share. Read the
    /// other way round it is a use-after-free on one side and a leak on the other.
    /// @return True when nothing outstanding remains.
    [[nodiscard]] bool cancel() noexcept;

  private:
    void* _handle = nullptr;
};

} // namespace core::net::detail
