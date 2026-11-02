// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

namespace vtbackend::test
{

/// Deflates @p input into a zlib stream (RFC 1950), as an application sending a kitty `o=z` payload would.
/// @param input The bytes to compress.
/// @return The complete zlib stream.
[[nodiscard]] inline std::vector<uint8_t> deflateZlib(std::span<uint8_t const> input)
{
    auto out = std::vector<uint8_t>(compressBound(static_cast<uLong>(input.size())));
    auto outSize = static_cast<uLongf>(out.size());
    auto const status =
        compress2(out.data(), &outSize, input.data(), static_cast<uLong>(input.size()), Z_BEST_COMPRESSION);
    REQUIRE(status == Z_OK);
    out.resize(outSize);
    return out;
}

/// Deflates @p input and base64-encodes the stream: a kitty `o=z` payload as it appears on the wire.
/// @param input The bytes to compress.
/// @return The base64 text of the zlib stream.
[[nodiscard]] inline std::string zlibBase64(std::string_view input)
{
    auto const source = std::vector<uint8_t>(input.begin(), input.end());
    auto const compressed = deflateZlib(source);
    return core::base64::encode(compressed.begin(), compressed.end());
}

} // namespace vtbackend::test
