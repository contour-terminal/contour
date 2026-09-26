// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `core::net::contract` — the socket contract's tripwires: the guards that turn the three rules a
/// transport must obey from prose into something that fails when it is broken.
///
/// **They are public, and that is the point.** Every rule here was stated somewhere unreachable
/// from the place it has to be obeyed — one in a comment about one file's own watcher, one in a
/// single caller that had worked it out for itself — and so the seventh transport, and the second
/// caller, got no warning at all. A transport outside this library (a decorator, a consumer's
/// test double) is under exactly the same rules as the ones shipped here, so it gets exactly the
/// same tripwires.
///
/// **The two slot guards end the process in EVERY build; the other two are Debug-only.** A second
/// operation armed over a parked one used to be an `assert` too, and under `NDEBUG` it displaced the
/// parked one, which was then never resumed: a hang with no message, in exactly the builds that
/// ship. A contract violation must not become a silent hang in any build, so the slot guards now
/// terminate, naming the direction and the handle, in Debug and Release alike. Neither of the
/// alternatives keeps a violation both loud and harmless: resolving the DISPLACED operation with an
/// error fails a live, healthy operation for its caller's bug -- the socket cannot tell a stale
/// parked wait from a live one at the arm site (see @c claimReadSlot) -- and refusing the NEW one
/// needs every transport's every verb to grow a refusal path that exists only for a caller's bug.
/// The fix for a caller that trips one belongs at the caller; what belongs here is the thing that
/// names it, which a terminating guard does in every build and an `assert` did in one.
/// `requireReadBuffer` (a false EOF, not a hang) and `assertTeardownIsSerialisedWithDispatch`
/// remain assertions. Each is watched refusing by a canary process that drives a REAL socket, because
/// asserting the assertion would prove `assert` works and say nothing about whether a transport ever reaches
/// it. Each canary is judged on a marker naming its own mode, printed immediately before the
/// guarded call -- see `SocketContractCanary.cpp` for why that, and not `WILL_FAIL`.
///
/// Origin: fastcached `Net/ReadSlot.hpp`, `Net/WriteSlot.hpp` and `Net/ISocket.hpp`'s
/// `Detail::RequireReadBuffer`, at `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`.

#include <core/Assert.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace core::net
{
class EventLoop;
}

namespace core::net::contract
{

/// Refuses to destroy a loop-owned object where the destruction would race the loop's readiness
/// dispatch (design spec §2 rule G5).
///
/// **Clearing a pending operation races the dispatch, and the race is silent.** A socket, listener
/// or dial belonging to a loop is destroyed either on that loop's worker thread — where no dispatch
/// can be running concurrently, because dispatching is what that thread is doing — or with the loop
/// stopped, where there is nothing to race. Any other thread, while a turn has not returned, is the
/// violation.
///
/// Declared here rather than inlined at each destructor so the rule has one spelling; the loop
/// answers the two facts (@c EventLoop::teardownIsSerialisedWithDispatch) and this states the rule
/// over them. Out of line so this header stays free of `<core/net/EventLoop.hpp>`, which a
/// consumer that only wants @c requireReadBuffer should not have to compile.
///
/// Debug-only, like every other guard here. Origin:
/// [fastcached#668](https://github.com/LASTRADA-Software/fastcached/issues/668).
/// @param loop The loop the object being destroyed belongs to.
void assertTeardownIsSerialisedWithDispatch(EventLoop const& loop) noexcept;

/// Refuses a `read` whose destination span is empty, at the transport that was handed it.
///
/// **An empty buffer is answered EOF, which is the one false claim `read`'s `0` exists to make
/// meaningful.** Every transport computes its result from what its receive primitive returned, and
/// every receive primitive answers `0` for a zero-length request: `recv(fd, p, 0, 0)` returns 0 on
/// every POSIX backend, a zero-length `WSARecv` completes with `bytesReceived == 0`, an in-memory
/// transport pulls nothing. And `0` on this interface means *the peer has finished sending*. So a
/// caller that reaches `read` with an empty span is told its peer closed
/// ([fastcached#838](https://github.com/LASTRADA-Software/fastcached/issues/838)).
///
/// **Nothing exotic gets a caller there.** An off-by-one in a `subspan(got)` accumulation loop, or
/// a decorator narrowing a chunk size to zero, and the loop terminates cleanly reporting a graceful
/// close that never happened.
///
/// **It is a programmer error, not a legitimate no-op**, which is why it is an assertion rather
/// than a @c NetErrorCode: a zero-byte read has no result this interface can express, because `0`
/// is taken and it is taken by the opposite fact. With assertions compiled out an empty read still
/// answers EOF, exactly as it did before this existed.
///
/// Watched refusing by `ctest -R empty-read-buffer-canary`.
/// @param buffer The destination span a caller passed to `read`.
inline void requireReadBuffer([[maybe_unused]] std::span<std::byte> buffer) noexcept
{
    assert(!buffer.empty()
           && "read was given an empty buffer: a zero-length read resolves to 0, which on this "
              "interface means the peer has finished sending, so the caller is handed a graceful "
              "close that never happened (see core/net/SocketContract.hpp and fastcached#838)");
}

/// Which of a socket's two operation slots a guard is about.
enum class SlotDirection : std::uint8_t
{
    Read,  ///< The read-op slot: `read`, `readWithFd` and `waitReadable`.
    Write, ///< The write-op slot: `write` and `writeVectored`.
};

/// Ends the process: a second operation was armed over a parked one in @p direction. Goes through
/// `core::detail::fail`, so a program's fail handler (`core::setFailHandler`) logs it before the
/// abort.
/// @param direction Which slot was taken twice.
/// @param handle What the handle is, as text, or empty where the socket has none.
/// @param where The guard's call site.
[[noreturn]] inline void secondOperationArmed(SlotDirection direction,
                                              std::string_view handle,
                                              std::source_location where) noexcept
{
    auto const isRead = direction == SlotDirection::Read;
    auto const verb = isRead ? std::string_view { "read" } : std::string_view { "write" };
    auto const origin =
        isRead ? std::string_view { "fastcached#663" } : std::string_view { "fastcached#893" };
    core::detail::fail(
        std::format("a second {} operation was armed over a parked one on {}: a socket has one "
                    "{} operation at a time, and the parked one would never be resumed "
                    "(see core/net/SocketContract.hpp and {})",
                    verb,
                    handle.empty() ? std::string_view { "a socket with no native handle" } : handle,
                    verb,
                    origin),
        "Socket contract violated:",
        where.file_name(),
        static_cast<int>(where.line()));
}

/// @tparam Handle What the socket's native handle is; @c std::nullptr_t where it has none.
/// @param handle The handle, or `nullptr`.
/// @return @p handle as the text a guard names it by, or empty for `nullptr`.
template <typename Handle>
[[nodiscard]] std::string describeHandle(Handle handle)
{
    if constexpr (std::is_null_pointer_v<Handle>)
        return {};
    else
        return std::format("handle {}", handle);
}

/// Takes a socket's single read-op slot for an operation that is about to park.
///
/// **A socket has ONE read operation, and `read`, `readWithFd` and `waitReadable` share it.** Every
/// reactor socket keeps one in-flight operation per direction, and all three read verbs begin by
/// claiming it. So arming any of them while another is parked drops the parked awaitable: that
/// coroutine is never resumed and never freed — one leaked frame plus everything it captured, per
/// occurrence, with no assertion, no error and no log, and a leak proportional to traffic on
/// whatever path did it ([fastcached#663](https://github.com/LASTRADA-Software/fastcached/issues/663)).
///
/// **Assignment and check are one expression on purpose.** Each call site used to spell the claim
/// as a bare `awaitable = nullptr;`, which is a line to forget the guard on — at the seventh site
/// as at the first. There is no such line left: the clear happens here or it does not happen.
///
/// **The hazard is the SITE, not ownership.** At the arm site a socket cannot tell a stale parked
/// wait from a live one, and cancelling a live one resolves its waiter as *the peer went away*,
/// dropping a healthy client. The CALLER can tell, because it knows when its own iteration ended
/// — which is why @c ISocket::cancelRead is a verb the caller spells and not something `read` does
/// on its behalf.
///
/// **It ends the process in every build** (see the file comment for why neither refusing nor
/// resolving is the answer), naming the direction and @p handle.
///
/// Watched refusing by `ctest -R socket-contract-canary.read-slot`, which double-arms a REAL socket,
/// on Debug and Release legs alike.
/// @tparam Slot What the socket keeps its parked read in. `void` where the two read verbs resolve
///         to different types and the socket tracks which one it armed.
/// @tparam Handle The socket's native handle type, or @c std::nullptr_t where it has none.
/// @param slot The socket's in-flight read pointer, cleared by this call.
/// @param handle The socket's native handle, for the message; `nullptr` where it has none.
/// @param where The call site, for the message.
template <typename Slot, typename Handle = std::nullptr_t>
inline void claimReadSlot(Slot*& slot,
                          Handle handle = nullptr,
                          std::source_location where = std::source_location::current()) noexcept
{
    if (slot != nullptr)
        secondOperationArmed(SlotDirection::Read, describeHandle(handle), where);
    slot = nullptr;
}

/// Takes a socket's single write-op slot for an operation that is about to park.
///
/// **The read-slot rule's missing half.** Every reactor socket keeps one in-flight operation *per
/// direction*, so the identical failure is reachable on the write side, and until this existed it
/// was reachable with none of the machinery that makes it observable on the read side
/// ([fastcached#893](https://github.com/LASTRADA-Software/fastcached/issues/893)). The rule was
/// written about reads because reads are where it was first observed, not because writes are
/// exempt.
///
/// **There is deliberately no `cancelWrite` counterpart.** @c ISocket::cancelRead exists because a
/// caller needed a spelling of *abandon* short of `close()`; no caller needs that on the write side
/// today, and inventing the verb before a caller needs it would be every transport writing `{}`
/// with no reason beside it. This is the tripwire only.
///
/// Ends the process in every build, as @c claimReadSlot does.
///
/// Watched refusing by `ctest -R socket-contract-canary.write-slot`, which double-arms a REAL
/// socket, on Debug and Release legs alike.
/// @tparam Slot What the socket keeps its parked write in.
/// @tparam Handle The socket's native handle type, or @c std::nullptr_t where it has none.
/// @param slot The socket's in-flight write pointer, cleared by this call.
/// @param handle The socket's native handle, for the message; `nullptr` where it has none.
/// @param where The call site, for the message.
template <typename Slot, typename Handle = std::nullptr_t>
inline void claimWriteSlot(Slot*& slot,
                           Handle handle = nullptr,
                           std::source_location where = std::source_location::current()) noexcept
{
    if (slot != nullptr)
        secondOperationArmed(SlotDirection::Write, describeHandle(handle), where);
    slot = nullptr;
}

} // namespace core::net::contract
