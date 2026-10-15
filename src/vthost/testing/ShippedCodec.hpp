// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The two hellos as contour 0.7.0 -- the first release with `contour daemon`, at codec revision 1 -- writes
/// and reads them: the oracle the tests hold every later revision's handshake to.
///
/// A daemon of that release keeps running across an upgrade, and it decodes a hello in full before it looks
/// at the version, refusing a byte too many or too few. Whatever a later revision sends it first must parse
/// the way these readers parse it, or the user meets a dropped connection instead of the version message.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <vthost/proto/Wire.hpp>

namespace vthost::testing
{

/// The codec revision contour 0.7.0 shipped.
constexpr auto ShippedCodecVersion = uint32_t { 1 };

/// The tag 0.7.0 gives the ClientHello.
constexpr auto ShippedClientHelloTag = uint64_t { 1 };
/// The tag 0.7.0 gives the ServerHello.
constexpr auto ShippedServerHelloTag = uint64_t { 2 };

/// Reads a ClientHello body as 0.7.0 reads one: the version, the token, a presence byte and, when that is not
/// zero, the session settings in that release's layout -- which has no sixel scrolling byte.
/// @param body A ClientHello frame's body.
/// @return The version the body states, or std::nullopt when 0.7.0 would refuse the frame.
[[nodiscard]] inline std::optional<uint32_t> shippedClientHelloVersion(std::span<std::byte const> body)
{
    auto in = proto::Reader { body };
    auto const version = in.u32();
    auto const settings = [&in] {
        auto const present = in.u8();
        if (!present || *present == 0)
            return present.has_value();
        if (!in.svarint() || !in.u8() || !in.u8() || !in.u8() || !in.u32() || !in.string())
            return false;
        auto const frozenModes = in.varint();
        return frozenModes.has_value()
               && std::ranges::all_of(std::views::iota(uint64_t { 0 }, *frozenModes), [&in](uint64_t) {
                      return in.varint().has_value() && in.u8().has_value();
                  });
    };
    if (!version || !in.string() || !settings() || in.remaining() != 0)
        return std::nullopt;
    return *version;
}

/// Reads a ServerHello body as 0.7.0 reads one: the version, and nothing after it.
/// @param body A ServerHello frame's body.
/// @return The version the body states, or std::nullopt when 0.7.0 would refuse the frame.
[[nodiscard]] inline std::optional<uint32_t> shippedServerHelloVersion(std::span<std::byte const> body)
{
    auto in = proto::Reader { body };
    auto const version = in.u32();
    if (!version || in.remaining() != 0)
        return std::nullopt;
    return *version;
}

/// A ClientHello frame as a 0.7.0 client writes one, stating session settings.
/// @param serial The frame's serial.
/// @return The frame's bytes.
[[nodiscard]] inline std::vector<std::byte> shippedClientHello(uint64_t serial)
{
    auto body = proto::Writer {};
    body.u32(ShippedCodecVersion);
    body.string("s3cr3t");
    body.u8(1);         // session settings follow
    body.svarint(-1);   // an unlimited history
    body.u8(41);        // VT420
    body.u8(1);         // grapheme clustering
    body.u8(1);         // reflow on resize
    body.u32(256);      // image registers
    body.string(" ./"); // word delimiters
    body.varint(1);     // one frozen mode ...
    body.varint(2027);  // ... grapheme clustering ...
    body.u8(1);         // ... frozen set
    auto frame = proto::Writer {};
    proto::writeFrame(frame, serial, ShippedClientHelloTag, body.view());
    return frame.take();
}

/// A ServerHello frame as a 0.7.0 daemon answers any hello: its version alone.
/// @param serial The frame's serial.
/// @return The frame's bytes.
[[nodiscard]] inline std::vector<std::byte> shippedServerHello(uint64_t serial)
{
    auto body = proto::Writer {};
    body.u32(ShippedCodecVersion);
    auto frame = proto::Writer {};
    proto::writeFrame(frame, serial, ShippedServerHelloTag, body.view());
    return frame.take();
}

} // namespace vthost::testing
