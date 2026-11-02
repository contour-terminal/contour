// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace vtbackend::kitty_graphics
{

/// Why a kitty graphics `o=z` payload could not be inflated.
enum class InflateError : uint8_t
{
    Corrupt,  ///< The input is not a complete, valid zlib stream.
    TooLarge, ///< The stream would inflate to more bytes than the caller allows.
};

/// Inflates a zlib stream, refusing to produce more than @p maxOutputBytes bytes.
///
/// The output is produced in fixed-size chunks and the cap is checked before each chunk is kept, so a
/// decompression bomb costs at most @p maxOutputBytes plus one chunk of memory, never its claimed size.
///
/// @param compressed Complete zlib stream (RFC 1950).
/// @param maxOutputBytes Largest inflated size to accept.
/// @return The inflated bytes, or why the stream was refused.
[[nodiscard]] std::expected<std::vector<uint8_t>, InflateError> inflateZlib(
    std::span<uint8_t const> compressed, size_t maxOutputBytes);

} // namespace vtbackend::kitty_graphics
