// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Two conversions every datagram suite needs: text in, text out.
///
/// Shared rather than written out in each suite. There is nothing subtle in either — which is the
/// point: two copies of a `reinterpret_cast` pair are two places to edit and one to forget.
///
/// Origin: fastcached `src/tests/DatagramPayload.hpp`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`).

#include <core/net/IDatagramSocket.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace core::net::testing
{

/// The bytes of @p text, as a datagram payload.
/// @param text The characters to send; it must outlive the span.
/// @return The same storage, as bytes.
[[nodiscard]] inline std::span<std::byte const> datagramBytes(std::string_view text)
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

/// @p datagram's payload as text.
/// @param datagram What arrived.
/// @return Its bytes, as a string.
[[nodiscard]] inline std::string datagramText(ReceivedDatagram const& datagram)
{
    return { reinterpret_cast<char const*>(datagram.payload.data()), datagram.payload.size() };
}

} // namespace core::net::testing
