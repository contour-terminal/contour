// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The two hellos as the codecs between contour 0.7.0 and version negotiation write and read them: codec 2
/// (command blocks, #1010) and codec 3 (status-line extensions, #2171). The oracle the tests hold the
/// negotiating hellos to, beside 0.7.0's own in ShippedCodec.hpp.
///
/// FROZEN, and written by hand from those revisions' layouts -- never derived from the current codec:
///
///  - codec 2 from `git show origin/master:src/vthost/proto/Pdu.cpp` (version 2 in its `Wire.hpp`);
///  - codec 3 from `git show 7bef185c:src/vthost/proto/Pdu.cpp` (version 3 in its `Wire.hpp`), the
///    head of PR #2171's branch.
///
/// Both codecs share one hello layout and differ only in the version they state. Each reads a hello's version
/// first; a hello of its own version it then reads to the last byte, and one of any other version it consumes
/// whole, unread, and refuses on the version. A daemon of either keeps running across an upgrade, so whatever
/// a negotiating peer sends it first must reach that refusal, not a decode error.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>

#include <vthost/proto/Wire.hpp>

namespace vthost::testing
{

/// The version codec 2 (command blocks) states in its hellos.
constexpr auto CommandBlockCodec = uint32_t { 2 };
/// The version codec 3 (status-line extensions) states in its hellos.
constexpr auto StatusLineExtensionCodec = uint32_t { 3 };

/// The tag codecs 2 and 3 give the ClientHello.
constexpr auto PreNegotiationClientHelloTag = uint64_t { 1 };
/// The tag codecs 2 and 3 give the ServerHello.
constexpr auto PreNegotiationServerHelloTag = uint64_t { 2 };

namespace detail
{
    /// Reads the session settings block as codecs 2 and 3 lay it out: a presence byte and, when that is not
    /// zero, the history limit, terminal id, grapheme clustering, reflow, image registers, sixel scrolling,
    /// word delimiters and frozen modes.
    /// @param in The reader, positioned at the presence byte.
    /// @return Whether the block parsed.
    [[nodiscard]] inline bool readPreNegotiationSessionSettings(proto::Reader& in)
    {
        auto const present = in.u8();
        if (!present)
            return false;
        if (*present == 0)
            return true;
        if (!in.svarint() || !in.u8() || !in.u8() || !in.u8() || !in.u32() || !in.u8() || !in.string())
            return false;
        auto const frozenModes = in.varint();
        return frozenModes.has_value()
               && std::ranges::all_of(std::views::iota(uint64_t { 0 }, *frozenModes), [&in](uint64_t) {
                      return in.varint().has_value() && in.u8().has_value();
                  });
    }

    /// Reads a ClientHello body as the codec stating @p ownVersion reads one.
    /// @param body A ClientHello frame's body.
    /// @param ownVersion The version of the codec reading it.
    /// @return The version the body states, or std::nullopt when that codec would refuse the frame.
    [[nodiscard]] inline std::optional<uint32_t> preNegotiationClientHelloVersion(
        std::span<std::byte const> body, uint32_t ownVersion)
    {
        auto in = proto::Reader { body };
        auto const version = in.u32();
        if (!version)
            return std::nullopt;
        if (*version != ownVersion)
        {
            std::ignore = in.bytes(in.remaining());
            return *version;
        }
        auto const fields = in.blob();
        if (!fields)
            return std::nullopt;
        auto inner = proto::Reader { *fields };
        if (!inner.string() || !readPreNegotiationSessionSettings(inner) || inner.remaining() != 0)
            return std::nullopt;
        auto const closing = in.u8();
        if (!closing || *closing != 0 || in.remaining() != 0)
            return std::nullopt;
        return *version;
    }

    /// Reads a ServerHello body as the codec stating @p ownVersion reads one: the version, and when it is
    /// that codec's own and anything follows, the daemon's machine id and host name.
    /// @param body A ServerHello frame's body.
    /// @param ownVersion The version of the codec reading it.
    /// @return The version the body states, or std::nullopt when that codec would refuse the frame.
    [[nodiscard]] inline std::optional<uint32_t> preNegotiationServerHelloVersion(
        std::span<std::byte const> body, uint32_t ownVersion)
    {
        auto in = proto::Reader { body };
        auto const version = in.u32();
        if (!version)
            return std::nullopt;
        if (*version != ownVersion)
        {
            std::ignore = in.bytes(in.remaining());
            return *version;
        }
        if (in.remaining() == 0)
            return *version;
        if (!in.string() || !in.string() || in.remaining() != 0)
            return std::nullopt;
        return *version;
    }

    /// A ClientHello frame as a client of the codec stating @p ownVersion writes one, stating session
    /// settings.
    /// @param serial The frame's serial.
    /// @param ownVersion The version of the codec writing it.
    /// @return The frame's bytes.
    [[nodiscard]] inline std::vector<std::byte> preNegotiationClientHello(uint64_t serial,
                                                                          uint32_t ownVersion)
    {
        auto fields = proto::Writer {};
        fields.string("s3cr3t");
        fields.u8(1);         // session settings follow
        fields.svarint(-1);   // an unlimited history
        fields.u8(41);        // VT420
        fields.u8(1);         // grapheme clustering
        fields.u8(1);         // reflow on resize
        fields.u32(256);      // image registers
        fields.u8(0);         // no sixel scrolling
        fields.string(" ./"); // word delimiters
        fields.varint(1);     // one frozen mode ...
        fields.varint(2027);  // ... grapheme clustering ...
        fields.u8(1);         // ... frozen set
        auto body = proto::Writer {};
        body.u32(ownVersion);
        body.blob(fields.view());
        body.u8(0);
        auto frame = proto::Writer {};
        proto::writeFrame(frame, serial, PreNegotiationClientHelloTag, body.view());
        return frame.take();
    }

    /// A ServerHello frame as a daemon of the codec stating @p ownVersion answers a hello of another version:
    /// its version alone.
    /// @param serial The frame's serial.
    /// @param ownVersion The version of the codec writing it.
    /// @return The frame's bytes.
    [[nodiscard]] inline std::vector<std::byte> preNegotiationServerHello(uint64_t serial,
                                                                          uint32_t ownVersion)
    {
        auto body = proto::Writer {};
        body.u32(ownVersion);
        auto frame = proto::Writer {};
        proto::writeFrame(frame, serial, PreNegotiationServerHelloTag, body.view());
        return frame.take();
    }
} // namespace detail

/// Reads a ClientHello body as a codec 2 daemon reads one.
/// @param body A ClientHello frame's body.
/// @return The version the body states, or std::nullopt when that daemon would refuse the frame.
[[nodiscard]] inline std::optional<uint32_t> codec2ClientHelloVersion(std::span<std::byte const> body)
{
    return detail::preNegotiationClientHelloVersion(body, CommandBlockCodec);
}

/// Reads a ServerHello body as a codec 2 client reads one.
/// @param body A ServerHello frame's body.
/// @return The version the body states, or std::nullopt when that client would refuse the frame.
[[nodiscard]] inline std::optional<uint32_t> codec2ServerHelloVersion(std::span<std::byte const> body)
{
    return detail::preNegotiationServerHelloVersion(body, CommandBlockCodec);
}

/// Reads a ClientHello body as a codec 3 daemon reads one.
/// @param body A ClientHello frame's body.
/// @return The version the body states, or std::nullopt when that daemon would refuse the frame.
[[nodiscard]] inline std::optional<uint32_t> codec3ClientHelloVersion(std::span<std::byte const> body)
{
    return detail::preNegotiationClientHelloVersion(body, StatusLineExtensionCodec);
}

/// Reads a ServerHello body as a codec 3 client reads one.
/// @param body A ServerHello frame's body.
/// @return The version the body states, or std::nullopt when that client would refuse the frame.
[[nodiscard]] inline std::optional<uint32_t> codec3ServerHelloVersion(std::span<std::byte const> body)
{
    return detail::preNegotiationServerHelloVersion(body, StatusLineExtensionCodec);
}

/// A ClientHello frame as a codec 2 client writes one, stating session settings.
/// @param serial The frame's serial.
/// @return The frame's bytes.
[[nodiscard]] inline std::vector<std::byte> codec2ClientHello(uint64_t serial)
{
    return detail::preNegotiationClientHello(serial, CommandBlockCodec);
}

/// A ServerHello frame as a codec 2 daemon answers a hello of another version.
/// @param serial The frame's serial.
/// @return The frame's bytes.
[[nodiscard]] inline std::vector<std::byte> codec2ServerHello(uint64_t serial)
{
    return detail::preNegotiationServerHello(serial, CommandBlockCodec);
}

/// A ClientHello frame as a codec 3 client writes one, stating session settings.
/// @param serial The frame's serial.
/// @return The frame's bytes.
[[nodiscard]] inline std::vector<std::byte> codec3ClientHello(uint64_t serial)
{
    return detail::preNegotiationClientHello(serial, StatusLineExtensionCodec);
}

/// A ServerHello frame as a codec 3 daemon answers a hello of another version.
/// @param serial The frame's serial.
/// @return The frame's bytes.
[[nodiscard]] inline std::vector<std::byte> codec3ServerHello(uint64_t serial)
{
    return detail::preNegotiationServerHello(serial, StatusLineExtensionCodec);
}

} // namespace vthost::testing
