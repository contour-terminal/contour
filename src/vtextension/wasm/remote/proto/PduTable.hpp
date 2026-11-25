// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one table that says, for every PDU kind, which channels it travels on, which way, how large its body
/// may be, and how it is decoded. A new kind is a new row here and a new alternative in Pdu.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <utility>
#include <variant>

#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/proto/Wire.hpp>

namespace vtextension::wasm::remote::proto
{

/// Decodes one PDU body of type @p Body: every field, then nothing more (`TrailingBytes` otherwise).
/// Defined, and instantiated for every Pdu alternative, in Codec.cpp.
/// @tparam Body The Pdu alternative decoded.
/// @param body The frame's body, after its kind.
/// @return The PDU; why the body is malformed.
template <typename Body>
[[nodiscard]] std::expected<Pdu, DecodeError> decodeBody(std::span<uint8_t const> body);

/// The channels a PDU kind may travel on: one bit per ChannelKind.
enum class ChannelSet : uint8_t
{
    Control = 1U << std::to_underlying(ChannelKind::Control), ///< The control channel alone.
    Lane = 1U << std::to_underlying(ChannelKind::Lane),       ///< A lane alone.
    Either = Control | Lane,                                  ///< Either: a lane, or the control channel.
};

/// What the protocol says about one PDU kind.
struct PduRow
{
    PduKind kind;                                                             ///< The kind described.
    ChannelSet channels;                                                      ///< The channels it travels on.
    Direction direction;                                                      ///< The way it travels.
    uint64_t (*sizeCap)(PduLimits const&);                                    ///< Its largest body, in bytes.
    std::expected<Pdu, DecodeError> (*decode)(std::span<uint8_t const> body); ///< Decodes its body.
};

/// One row per PduKind, in kind order: row `i` describes kind `i + 1`, and decodes Pdu alternative `i`.
///
/// A cap bounds a body in bytes, so that a reader refuses a frame from its header alone (Codec.hpp). Fixed
/// caps leave room above the largest honest body: five bytes per u32 varint, ten per u64. The caps of the
/// PDUs carrying a module, its descriptions or memory follow the stream's PduLimits.
inline constexpr auto PduTable = std::array {
    PduRow { .kind = PduKind::Hello,
             .channels = ChannelSet::Control,
             .direction = Direction::ToParent,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 16; },
             .decode = &decodeBody<Hello> },
    PduRow { .kind = PduKind::CreateRuntime,
             .channels = ChannelSet::Control,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const&) -> uint64_t { return uint64_t { 64 } * 1024; },
             .decode = &decodeBody<CreateRuntime> },
    PduRow { .kind = PduKind::RuntimeCreated,
             .channels = ChannelSet::Control,
             .direction = Direction::ToParent,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 16; },
             .decode = &decodeBody<RuntimeCreated> },
    PduRow { .kind = PduKind::Load,
             .channels = ChannelSet::Control,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const& limits) -> uint64_t { return limits.maxModuleBytes + 64; },
             .decode = &decodeBody<Load> },
    // Descriptions repeat the module's names and signatures, so they may outgrow the module somewhat.
    PduRow { .kind = PduKind::Loaded,
             .channels = ChannelSet::Control,
             .direction = Direction::ToParent,
             .sizeCap = [](PduLimits const& limits) -> uint64_t { return 4 * limits.maxModuleBytes; },
             .decode = &decodeBody<Loaded> },
    PduRow { .kind = PduKind::Instantiate,
             .channels = ChannelSet::Control,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 64; },
             .decode = &decodeBody<Instantiate> },
    PduRow { .kind = PduKind::Instantiated,
             .channels = ChannelSet::Control,
             .direction = Direction::ToParent,
             .sizeCap = [](PduLimits const& limits) -> uint64_t { return limits.maxModuleBytes; },
             .decode = &decodeBody<Instantiated> },
    PduRow { .kind = PduKind::Release,
             .channels = ChannelSet::Control,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 16; },
             .decode = &decodeBody<Release> },
    PduRow { .kind = PduKind::Call,
             .channels = ChannelSet::Lane,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 16 + (5 * MaxCallValues); },
             .decode = &decodeBody<Call> },
    PduRow { .kind = PduKind::CallHost,
             .channels = ChannelSet::Lane,
             .direction = Direction::ToParent,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 16 + (5 * MaxHostParameters); },
             .decode = &decodeBody<CallHost> },
    PduRow { .kind = PduKind::HostResult,
             .channels = ChannelSet::Lane,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 8; },
             .decode = &decodeBody<HostResult> },
    PduRow { .kind = PduKind::CallResult,
             .channels = ChannelSet::Lane,
             .direction = Direction::ToParent,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 16 + (5 * MaxCallValues); },
             .decode = &decodeBody<CallResult> },
    PduRow { .kind = PduKind::MemoryRead,
             .channels = ChannelSet::Lane,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 32; },
             .decode = &decodeBody<MemoryRead> },
    PduRow { .kind = PduKind::MemoryData,
             .channels = ChannelSet::Lane,
             .direction = Direction::ToParent,
             .sizeCap = [](PduLimits const& limits) -> uint64_t { return limits.maxMemoryBytes + 16; },
             .decode = &decodeBody<MemoryData> },
    PduRow { .kind = PduKind::MemoryWrite,
             .channels = ChannelSet::Lane,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const& limits) -> uint64_t { return limits.maxMemoryBytes + 32; },
             .decode = &decodeBody<MemoryWrite> },
    // Memory grows during a lane's call, and during instantiation, in a start function, on the control
    // channel; it is answered on the channel the Grow arrived on.
    PduRow { .kind = PduKind::Grow,
             .channels = ChannelSet::Either,
             .direction = Direction::ToParent,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 16; },
             .decode = &decodeBody<Grow> },
    PduRow { .kind = PduKind::Grown,
             .channels = ChannelSet::Either,
             .direction = Direction::ToHelper,
             .sizeCap = [](PduLimits const&) -> uint64_t { return 8; },
             .decode = &decodeBody<Grown> },
};

/// @return Whether a PDU of @p row's kind may travel on @p channel.
[[nodiscard]] constexpr bool travelsOn(PduRow const& row, ChannelKind channel) noexcept
{
    return (std::to_underlying(row.channels) & (1U << std::to_underlying(channel))) != 0;
}

static_assert(PduTable.size() == std::variant_size_v<Pdu>, "one PduTable row per Pdu alternative");

static_assert(
    []<std::size_t... Index>(std::index_sequence<Index...>) {
        return ((std::to_underlying(PduTable[Index].kind) == Index + 1
                 && PduTable[Index].decode == &decodeBody<std::variant_alternative_t<Index, Pdu>>)
                && ...);
    }(std::make_index_sequence<PduTable.size()> {}),
    "row i describes kind i + 1 and decodes Pdu alternative i");

} // namespace vtextension::wasm::remote::proto
