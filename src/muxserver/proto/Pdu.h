// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native protocol's typed PDU catalog.
///
/// Tags are EXPLICIT stable integers: retired tags leave gaps, and an unknown
/// ident decodes to Invalid{ident} — data, not an error — so old servers and
/// new clients keep talking. Adding a PDU is: a struct, a tag in PduType, one
/// row in each of the encode/decode tables in Pdu.cpp.
///
/// The structs are deliberately wire-level (raw u32 colors, raw flag words,
/// std-only types): the codec is shared by the server, the attach client, and
/// tests without dragging vtbackend into every consumer.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <muxserver/proto/Wire.h>

namespace muxserver::proto
{

/// The stable wire tag of each PDU (the wire carries it as a varint; the enum's
/// base type only bounds the catalog, not the protocol).
enum class PduType : uint8_t
{
    Invalid = 0,
    ClientHello = 1,
    ServerHello = 2,
    Input = 3,
    ResizeRequest = 4,
    FetchImage = 5,
    ImageData = 6,
    ImageGone = 7,
    SessionState = 8,
    Delta = 9,
};

/// An ident this decoder does not know (yet). Carried, never fatal.
struct Invalid
{
    uint64_t ident = 0;
    bool operator==(Invalid const&) const = default;
};

/// Client's first PDU: its codec revision. Anything before it is a protocol error.
struct ClientHello
{
    uint32_t codecVersion = CodecVersion;
    bool operator==(ClientHello const&) const = default;
};

/// Server's answer to ClientHello.
struct ServerHello
{
    uint32_t codecVersion = CodecVersion;
    bool operator==(ServerHello const&) const = default;
};

/// Client keyboard/paste bytes for one session's PTY.
struct Input
{
    uint64_t session = 0;
    std::vector<std::byte> data;
    bool operator==(Input const&) const = default;
};

/// The client's size proposal; the server answers with authoritative state.
struct ResizeRequest
{
    uint32_t columns = 0;
    uint32_t lines = 0;
    bool operator==(ResizeRequest const&) const = default;
};

/// On-demand image pixel fetch by stable image id.
struct FetchImage
{
    uint32_t imageId = 0;
    bool operator==(FetchImage const&) const = default;
};

/// The image bytes for a FetchImage answer. Clients cache by id.
struct ImageData
{
    uint32_t imageId = 0;
    uint8_t format = 0;  ///< vtbackend::ImageFormat's underlying value.
    uint32_t width = 0;  ///< Pixels.
    uint32_t height = 0; ///< Pixels.
    std::vector<std::byte> data;
    bool operator==(ImageData const&) const = default;
};

/// FetchImage answer when the refcount already dropped the image: the client
/// clears the cells referencing it.
struct ImageGone
{
    uint32_t imageId = 0;
    bool operator==(ImageGone const&) const = default;
};

/// Everything renditional a session carries OUTSIDE its grid cells — replayed
/// on attach and after every ResyncRequired.
struct SessionState
{
    uint64_t session = 0;
    uint32_t columns = 0;
    uint32_t lines = 0;
    uint8_t screenType = 0; ///< 0 = primary, 1 = alternate.
    int32_t cursorLine = 0;
    int32_t cursorColumn = 0;
    uint8_t cursorShape = 0;
    uint8_t cursorVisible = 1;
    std::string title;
    uint32_t defaultForeground = 0; ///< Raw RGBA.
    uint32_t defaultBackground = 0; ///< Raw RGBA.
    std::vector<uint32_t> palette;  ///< Indexed colors, raw RGBA.
    bool operator==(SessionState const&) const = default;
};

/// One cell's full renditional state on the wire.
struct WireCell
{
    char32_t codepoint = 0;
    std::vector<char32_t> clusterExtras; ///< Extra codepoints of the grapheme cluster.
    uint8_t width = 1;
    uint8_t scale = 1;            ///< OSC 66 block height.
    uint16_t textScaleExtras = 0; ///< Packed fraction/alignment (TextScale.h).
    uint16_t hyperlink = 0;       ///< Server-side HyperlinkId; URI via the side table.
    uint32_t foreground = 0;      ///< Raw vtbackend::Color bits.
    uint32_t background = 0;
    uint32_t underlineColor = 0;
    uint32_t flags = 0; ///< Raw CellFlags bits.
    bool operator==(WireCell const&) const = default;
};

/// One grid row, addressed by its stable id.
struct WireLine
{
    int64_t stableId = 0;
    uint16_t flags = 0; ///< Raw LineFlags bits.
    uint32_t columns = 0;
    /// Empty for a blank line (uniformly filled with the fill attributes below).
    std::vector<WireCell> cells;
    uint32_t fillForeground = 0;
    uint32_t fillBackground = 0;
    bool operator==(WireLine const&) const = default;
};

/// id → URI, sent once per connection on first reference (immune to the
/// server-side hyperlink LRU evicting the id later).
struct HyperlinkEntry
{
    uint16_t id = 0;
    std::string uri;
    bool operator==(HyperlinkEntry const&) const = default;
};

/// One image-covered cell: which row/column shows which part of which image.
struct ImageCellEntry
{
    int64_t stableId = 0; ///< Row.
    uint16_t column = 0;
    uint32_t imageId = 0;
    uint16_t offsetLine = 0; ///< Fragment offset within the image, in cells.
    uint16_t offsetColumn = 0;
    uint8_t layer = 0;
    bool operator==(ImageCellEntry const&) const = default;
};

/// A batch of changed rows plus the side tables they reference. `snapshot`
/// marks a full resync (attach or generation change) rather than an increment.
struct Delta
{
    uint64_t session = 0;
    uint64_t generation = 0;
    uint64_t seqno = 0;
    uint8_t snapshot = 0;
    /// The stable id of page row 0 at delta time — what lets the client map
    /// stable-id-addressed rows onto its viewport.
    int64_t stableViewportBase = 0;
    int32_t cursorLine = 0;
    int32_t cursorColumn = 0;
    std::vector<WireLine> lines;
    std::vector<HyperlinkEntry> hyperlinks;
    std::vector<ImageCellEntry> imageCells;
    bool operator==(Delta const&) const = default;
};

using DecodedPdu = std::variant<Invalid,
                                ClientHello,
                                ServerHello,
                                Input,
                                ResizeRequest,
                                FetchImage,
                                ImageData,
                                ImageGone,
                                SessionState,
                                Delta>;

/// Encodes @p pdu (body + frame) into @p sink.
/// @param sink The output writer.
/// @param serial Request correlation; 0 = unsolicited push.
/// @param pdu Any catalog PDU.
void encodePdu(Writer& sink, uint64_t serial, DecodedPdu const& pdu);

/// The result of decoding one frame's worth of input.
struct DecodedFrame
{
    uint64_t serial = 0;
    DecodedPdu pdu;
    std::size_t consumed = 0; ///< Input bytes to drop from the stream.
};

/// Decodes the next PDU from @p data; NeedMoreData while the frame is incomplete.
[[nodiscard]] std::expected<DecodedFrame, DecodeError> decodePdu(std::span<std::byte const> data);

} // namespace muxserver::proto
