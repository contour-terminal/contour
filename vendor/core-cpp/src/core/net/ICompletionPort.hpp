// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ICompletionPort` — what a completion-based @c IoBackend lends to the sockets that
/// sit on it, and the one place a handle is associated with a port.
///
/// A readiness backend keeps everything it knows to itself: a caller hands it a
/// descriptor and gets a callback back. A completion-based one cannot, because the
/// operations are issued by the SOCKET (`WSARecv`, `WSASend`, `AcceptEx`, `ConnectEx`)
/// and completed by the PORT, so the socket has to be able to name the port it will be
/// completed on. This interface is that seam, and it is deliberately narrow: it lends
/// the port, it does not lend the backend.
///
/// Declared on every platform although only Windows implements it. A member only one
/// platform uses is still declared on all of them (`.agent/rules/platform.md`) — a
/// header that changes shape per platform is one a consumer's build can disagree with
/// this one about, and the `#if defined(_WIN32)` the design spec sketches
/// @c IoBackend::completionPort behind would do exactly that. Nothing of Windows
/// appears here: a handle is @c platform::NativeHandle, which is `void*` there and an
/// `int` elsewhere.

#include <core/net/NetError.hpp>
#include <core/platform/Types.hpp>

#include <expected>

namespace core::net
{

/// The completion port a backend dequeues, as much of it as anything outside the
/// backend may touch.
///
/// Lifetime: owned by its backend, and valid exactly as long as the backend is. A
/// socket that outlived the loop it was created on would already be a violation of
/// guarantee G5, which every socket destructor asserts.
class ICompletionPort
{
  public:
    ICompletionPort() = default;

    ICompletionPort(ICompletionPort const&) = delete;
    ICompletionPort& operator=(ICompletionPort const&) = delete;
    ICompletionPort(ICompletionPort&&) = delete;
    ICompletionPort& operator=(ICompletionPort&&) = delete;

    /// Associates @p handle with this port, so its overlapped operations complete here.
    ///
    /// **Guarantee G4: a SOCKET is associated with exactly one port, and this is the
    /// only place it happens.** The association is a property of the handle and cannot
    /// be undone, changed or queried: `CreateIoCompletionPort` on an already-associated
    /// handle fails with `ERROR_INVALID_PARAMETER`, which is what it also answers for a
    /// closed handle and for half a dozen ordinary mistakes, so a caller reading that
    /// back would condemn a working connection. The port therefore keeps the record
    /// itself, refuses a second association by name, and asserts on it — two owners
    /// each believing they hold the association is a defect wherever it happens, and
    /// the loser then awaits completions that are delivered to the winner.
    ///
    /// @param handle The socket or file handle to associate (not owned).
    /// @return Nothing on success; @c NetErrorCode::BadHandle for an invalid handle,
    ///         @c NetErrorCode::AddressInUse when this handle is already associated
    ///         with this port, or @c NetErrorCode::SystemError carrying
    ///         `GetLastError()` when the kernel refused.
    [[nodiscard]] virtual std::expected<void, NetError> associate(platform::NativeHandle handle) = 0;

    /// @return Whether @p handle has already been associated with this port, which is
    ///         what lets a caller that associated it itself say so instead of asking
    ///         for a second association it would be refused.
    /// @param handle The handle to ask about.
    [[nodiscard]] virtual bool isAssociated(platform::NativeHandle handle) const noexcept = 0;

    /// Drops @p handle from the record, which the owner of a handle it is CLOSING must
    /// do.
    ///
    /// **Not an obligation the port can discharge for you, and not an optional
    /// tidy-up.** An association ends when the handle is closed and the kernel says
    /// nothing about it; the operating system then reuses handle values freely, so a
    /// record left standing makes the NEXT socket to be handed that value look already
    /// associated. It is then never associated at all, and every operation issued on it
    /// completes nowhere — a hang with no error and no log line. Closing without this
    /// is the one way to get that, so it is stated here rather than in a guide.
    /// @param handle The handle being closed.
    virtual void forget(platform::NativeHandle handle) noexcept = 0;

    /// Records that an owner is about to hand the kernel @p operation on this port, so the
    /// completion it produces is routed back rather than dropped.
    ///
    /// **Called BEFORE the Winsock call, and withdrawn if that call fails outright.** A
    /// completion can be queued the instant the call is issued, and a port that did not
    /// yet know the pointer would have nothing to recognise it by. A port serves
    /// everything associated with it, so without this record a packet naming an owner's
    /// operation is indistinguishable from one naming the backend's own, and casting one
    /// to the other is a use-after-free with extra steps.
    ///
    /// What the port does with it, stated where an owner reads it: when the packet is
    /// dequeued the operation is marked completed, a @c HandleKind::Completion
    /// registration naming it is reported readable, and the operation's own dequeue hook
    /// runs LAST — after which the port never touches the pointer again, because the hook
    /// is where the owner gives back the share that kept the operation alive while the
    /// kernel held it ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)).
    ///
    /// The pointer is typed `void*` so this header stays portable; it is the address of a
    /// `core::net::detail::IocpOperation` (`windows/IocpOperation.hpp`), whose first member is
    /// the `OVERLAPPED` the kernel is handed.
    /// @param operation The operation about to be issued.
    virtual void beginOperation(void* operation) = 0;

    /// Takes back @c beginOperation for an operation the kernel refused synchronously, so
    /// no completion will ever arrive for it.
    /// @param operation The operation that was not issued after all.
    virtual void withdrawOperation(void* operation) noexcept = 0;

    /// The raw port, for the two Winsock calls that take one by name (`AcceptEx` and
    /// `ConnectEx` need the association in place before they are issued, and a
    /// diagnostic sometimes wants the number).
    ///
    /// **Not an invitation to dequeue it.** Guarantee G1 says exactly one thread takes
    /// completions off this port, and that thread is the loop's; a second
    /// `GetQueuedCompletionStatus` on this handle steals a packet the loop is the only
    /// one that knows how to route.
    /// @return The port handle.
    [[nodiscard]] virtual platform::NativeHandle nativeHandle() const noexcept = 0;

  protected:
    /// Non-virtual and protected: a caller borrows a port, never owns one.
    ~ICompletionPort() = default;
};

} // namespace core::net
