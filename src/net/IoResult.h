// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Result and error types for the async socket layer. All fallible socket I/O
/// resolves to an @c IoResult (`std::expected<std::size_t, NetError>`): the value
/// is the number of bytes transferred (0 on a clean EOF for reads), the error is a
/// structured @c NetError.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace net
{

/// Classifies a network failure. Mirrors the categories the daemon and its
/// clients need to distinguish (clean shutdown vs cancellation vs a real
/// transport error).
enum class NetErrorCode : std::uint8_t
{
    Ok = 0,          ///< No error (not normally stored in an error result).
    Eof,             ///< The peer closed the connection cleanly.
    Cancelled,       ///< The operation was cancelled (stop requested / listener closed).
    Timeout,         ///< A deadline elapsed before the operation completed.
    WouldBlock,      ///< The operation would block (transient; the reactor retries).
    BadHandle,       ///< The socket/handle is closed or invalid.
    ConnReset,       ///< The connection was reset by the peer.
    ConnRefused,     ///< A connect was refused.
    AddressInUse,    ///< A bind failed because the address is in use.
    AddressError,    ///< Address resolution or parsing failed.
    Unsupported,     ///< The operation is not supported on this platform/transport.
    MessageTooLarge, ///< A framed unit (line, PDU) exceeded its configured bound.
    Other,           ///< An unclassified OS error (see systemCode).
};

/// @param code The error code to describe.
/// @return A short human-readable description of @p code.
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
        case NetErrorCode::AddressError: return "address error";
        case NetErrorCode::Unsupported: return "unsupported";
        case NetErrorCode::MessageTooLarge: return "message too large";
        case NetErrorCode::Other: return "network error";
    }
    return "unknown error";
}

/// A structured network error: a category, the raw OS error number (errno /
/// WSAGetLastError, 0 if none), and an optional context string for diagnostics.
struct NetError
{
    NetErrorCode code = NetErrorCode::Other; ///< The error category.
    int systemCode = 0;                      ///< The raw OS error number, or 0.
    std::string context;                     ///< Optional human context (e.g. the failing call).

    /// @return A descriptive string combining the category, context, and OS code.
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

/// Result of a byte-transfer operation: the count transferred, or a @c NetError.
using IoResult = std::expected<std::size_t, NetError>;

} // namespace net
