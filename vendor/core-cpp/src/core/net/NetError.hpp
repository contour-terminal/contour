// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The error vocabulary of the async socket layer: a @c NetErrorCode category, the predicate
/// @c isDeadlineExpiry over it, and a structured @c NetError that adds the OS error number and a
/// context string. @c IoResult (`<core/net/IoResult.hpp>`) is the result type built on it.
///
/// This header is `core::net_types`, which links nothing and is compiled into every consumer that
/// touches the net layer at all, so it keeps its include set to what it uses. `<format>` in
/// particular is not free.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace core::net
{

/// Classifies a network failure. The union of the two vocabularies core-cpp's net layer was merged
/// from — contour's `net::NetErrorCode` and fastcached's `FastCache::NetErrorCode` — so a caller of
/// either lineage still has a code for every failure it used to distinguish.
enum class NetErrorCode : std::uint8_t
{
    Ok = 0,           ///< No error. Not stored in an error result; it is what a `NetErrorCode`
                      ///< variable holds before anything has failed.
    Eof,              ///< The peer finished sending (it closed its write side cleanly).
    Cancelled,        ///< The operation was cancelled by the resource (close, cancelRead, a closed
                      ///< listener). A cancel from the flow's own stop token throws instead.
    Timeout,          ///< A deadline elapsed before the operation completed.
    WouldBlock,       ///< The operation would block (transient; the backend reports readiness).
    BadHandle,        ///< The socket, descriptor or handle is closed or invalid.
    ConnReset,        ///< The peer reset the connection mid-flight.
    ConnRefused,      ///< A connect was refused by the peer.
    AddressInUse,     ///< A bind failed because the endpoint is taken.
    AddressNotAvail,  ///< A bind failed because the address is not available locally.
    AddressError,     ///< Address resolution or parsing failed.
    HostUnreach,      ///< The network reports the destination as unreachable.
    PermissionDenied, ///< The OS refused the operation (a low-numbered port without privileges, a
                      ///< firewall's EACCES).
    Unsupported,      ///< The operation is not supported on this platform or transport.
    MessageTooLarge,  ///< A framed unit (line, PDU, datagram) exceeded its configured bound.
    SystemError,      ///< An OS error nothing classified further; inspect `NetError::systemCode`.

    Last, ///< Not a code: the number of codes above it, so a table or a test can cover every one of
          ///< them without restating the list. Never constructed, never returned, never compared
          ///< against a result. **A new code goes above it, never below.** One appended after
          ///< `Last` still satisfies the switch and still leaves `Last` looking like a count, and
          ///< every case that walks `[0, Last)` would miss it; `NetError_test.cpp`'s
          ///< "No code hides above Last" is what refuses that, and it is the only thing that does.
};

/// @param code The error code to describe.
/// @return A short human-readable description of @p code, or `"unknown error"` for a value that is
///         not one of the codes (including `Last`).
///
/// The switch has no `default`, deliberately: adding a code then makes every compiler name this
/// function, which is how a new code is stopped from silently rendering as `"unknown error"` in
/// every log line that carries it. Do not add one. The statement after the switch handles the
/// values that are not enumerators, which a cast can still produce.
///
/// A description is lower-case words separated by single spaces, with no punctuation and no
/// capital: these strings end up in log lines that people grep, so one that shouts or ends in a
/// full stop changes the shape of every line carrying that code. `NetError_test.cpp` enforces it
/// rather than trusting the next author to notice the pattern.
[[nodiscard]] constexpr std::string_view toString(NetErrorCode code) noexcept
{
    switch (code)
    {
        case NetErrorCode::Ok: return "ok";
        case NetErrorCode::Eof: return "end of stream";
        case NetErrorCode::Cancelled: return "cancelled";
        case NetErrorCode::Timeout: return "timed out";
        case NetErrorCode::WouldBlock: return "would block";
        case NetErrorCode::BadHandle: return "bad handle";
        case NetErrorCode::ConnReset: return "connection reset";
        case NetErrorCode::ConnRefused: return "connection refused";
        case NetErrorCode::AddressInUse: return "address in use";
        case NetErrorCode::AddressNotAvail: return "address not available";
        case NetErrorCode::AddressError: return "address error";
        case NetErrorCode::HostUnreach: return "host unreachable";
        case NetErrorCode::PermissionDenied: return "permission denied";
        case NetErrorCode::Unsupported: return "unsupported";
        case NetErrorCode::MessageTooLarge: return "message too large";
        case NetErrorCode::SystemError: return "system error";
        case NetErrorCode::Last: break;
    }
    return "unknown error";
}

/// Whether a failed operation failed because its deadline expired.
///
/// **Two codes, one fact, and which one arrives is the platform's choice.** A receive or send
/// deadline armed with `SO_RCVTIMEO`/`SO_SNDTIMEO`, and a poll given a timeout, expire as
/// `EAGAIN`/`EWOULDBLOCK` on POSIX and as `WSAETIMEDOUT` on Winsock: `WouldBlock` here and
/// `Timeout` there. A caller asking "did I run out of time" has to accept both, and one that spells
/// only the obvious operand is correct on one platform and silently wrong on the other.
///
/// **Both operands are load-bearing, and neither may be dropped.** The callers this exists for are
/// the accept loops of the blocking transports, whose listener arms a poll timeout and whose loop
/// reads an expiry as *the poll ticked; re-check the stop flag and accept again*. Nothing in
/// core-cpp asks this yet: those transports arrive with Task B9, and the predicate is here now
/// because it belongs to the vocabulary rather than to them. Narrow this to
/// `Timeout` alone and each of those loops treats every POSIX tick as a fatal accept error, logs
/// once and returns: the server stops accepting about a quarter of a second after it starts, with
/// one `Debug` line as the only symptom. That is worth spelling out because the obvious mental
/// model invites exactly that edit — "a deadline expiring" sounds like a *timeout* and `WouldBlock`
/// sounds like *would have blocked, try again*, so the two look like different questions and are
/// not. On a socket or a poll with a deadline armed they are one event under two names, which is
/// the whole reason this predicate exists. Measured in fastcached, where two accept loops in two
/// subsystems asked the question open-coded:
/// [fastcached#824](https://github.com/LASTRADA-Software/fastcached/issues/824).
///
/// A caller that arms no deadline may test `WouldBlock` alone, and should say at that site that the
/// reason is reachability — `Timeout` cannot arrive there — and not semantics, or the next reader
/// files the narrow test as a defect.
///
/// It says nothing about *whose* deadline. A caller that must tell "I gave up" from "the peer went
/// away" asks its own timer; expiry closes the socket, so the two reach it as one broken socket
/// (`.agent/rules/async-and-net.md`, "Sockets").
///
/// @param code The code an operation failed with.
/// @return Whether that code is this platform's spelling of a deadline expiry.
[[nodiscard]] constexpr bool isDeadlineExpiry(NetErrorCode code) noexcept
{
    return code == NetErrorCode::Timeout || code == NetErrorCode::WouldBlock;
}

/// A structured network error: a category, the raw OS error number (errno /
/// WSAGetLastError, 0 if none), and an optional context string for diagnostics.
struct NetError
{
    NetErrorCode code = NetErrorCode::SystemError; ///< The error category.
    int systemCode = 0;                            ///< The raw OS error number, or 0.
    std::string context;                           ///< Optional human context (e.g. the failing call).

    /// Renders the error as words, never as an enumerator's position.
    ///
    /// The rejected alternative is fastcached's `NetError(code=9 system=104 context=recv)`, and the
    /// reason it is rejected leaves no trace when it bites: `code=` is an index into
    /// @c NetErrorCode, and the merge that produced this enumeration renumbered it. A line written
    /// before that merge and a line written after it are then identical character for character and
    /// mean different codes, so a reader comparing two runs, or a filter written against the old
    /// numbering, is wrong with nothing to notice. Words cannot fail that way, and they cost a
    /// reader no copy of this header. Keeping @c std::format out is the smaller reason: this is
    /// `core::net_types`, which links nothing.
    /// @return A descriptive string combining the category, context, and OS code, for example
    ///         `connection reset (recv) [errno 104]`.
    [[nodiscard]] std::string toString() const
    {
        auto result = std::string { net::toString(code) };
        if (!context.empty())
            result += " (" + context + ")";
        if (systemCode != 0)
            result += " [errno " + std::to_string(systemCode) + "]";
        return result;
    }
};

/// Builds a @c NetError.
/// @param code The error category.
/// @param systemCode The raw OS error number, or 0.
/// @param context Optional diagnostic context.
/// @return The assembled error.
[[nodiscard]] inline NetError makeNetError(NetErrorCode code, int systemCode = 0, std::string context = {})
{
    return NetError { .code = code, .systemCode = systemCode, .context = std::move(context) };
}

} // namespace core::net
