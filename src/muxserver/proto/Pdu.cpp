// SPDX-License-Identifier: Apache-2.0
#include <muxserver/proto/Pdu.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <ranges>
#include <utility>
#include <vector>

namespace muxserver::proto
{

namespace
{
    /// Assigns a checked read into @p out (narrowing deliberately, the wire type
    /// is the authority), or records the error.
    template <typename T, typename U>
    [[nodiscard]] bool assign(std::expected<U, DecodeError> value, T& out, DecodeError& error)
    {
        if (!value)
        {
            error = value.error();
            return false;
        }
        out = static_cast<T>(*value);
        return true;
    }

    /// Reserves room for @p count elements, but never more than the reader's
    /// remaining bytes could possibly back — each element costs at least one
    /// byte on the wire. A lie in @p count therefore cannot trigger a giant
    /// allocation (bad_alloc/length_error escaping the std::expected path);
    /// the decode loop still runs to @p count and trips MalformedPdu on the
    /// short body instead.
    template <typename T>
    void reserveBounded(std::vector<T>& out, std::size_t count, Reader const& in)
    {
        out.reserve(std::min<std::size_t>(count, in.remaining()));
    }

    /// Decodes a varint-length-prefixed vector into @p out: the count, a bounded
    /// reserve (see reserveBounded), then that many elements via @p decodeElement.
    /// Collapses the count/reserve/loop/push_back block that every length-prefixed
    /// field otherwise repeats verbatim, so the shared decode discipline lives once.
    /// @return The DecodeError of the count read or the first failing element.
    template <typename T, typename DecodeElement>
    [[nodiscard]] std::expected<void, DecodeError> decodeVector(Reader& in,
                                                                std::vector<T>& out,
                                                                DecodeElement decodeElement)
    {
        auto count = std::size_t {};
        auto error = DecodeError {};
        if (!assign(in.varint(), count, error))
            return std::unexpected(error);
        reserveBounded(out, count, in);
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        {
            auto element = decodeElement(in);
            if (!element)
                return std::unexpected(element.error());
            out.push_back(std::move(*element));
        }
        return {};
    }

    // --- the wire tag of each PDU type (the encode half of the catalog) -----

    [[nodiscard]] constexpr uint64_t tagOf(Invalid const& pdu) noexcept
    {
        return pdu.ident;
    }
    [[nodiscard]] constexpr uint64_t tagOf(ClientHello const&) noexcept
    {
        return std::to_underlying(PduType::ClientHello);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ServerHello const&) noexcept
    {
        return std::to_underlying(PduType::ServerHello);
    }
    [[nodiscard]] constexpr uint64_t tagOf(Input const&) noexcept
    {
        return std::to_underlying(PduType::Input);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ResizeRequest const&) noexcept
    {
        return std::to_underlying(PduType::ResizeRequest);
    }
    [[nodiscard]] constexpr uint64_t tagOf(FetchImage const&) noexcept
    {
        return std::to_underlying(PduType::FetchImage);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ImageData const&) noexcept
    {
        return std::to_underlying(PduType::ImageData);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ImageGone const&) noexcept
    {
        return std::to_underlying(PduType::ImageGone);
    }
    [[nodiscard]] constexpr uint64_t tagOf(SessionState const&) noexcept
    {
        return std::to_underlying(PduType::SessionState);
    }
    [[nodiscard]] constexpr uint64_t tagOf(Delta const&) noexcept
    {
        return std::to_underlying(PduType::Delta);
    }
    [[nodiscard]] constexpr uint64_t tagOf(SessionEvent const&) noexcept
    {
        return std::to_underlying(PduType::SessionEvent);
    }
    [[nodiscard]] constexpr uint64_t tagOf(LayoutState const&) noexcept
    {
        return std::to_underlying(PduType::LayoutState);
    }
    [[nodiscard]] constexpr uint64_t tagOf(CreateTab const&) noexcept
    {
        return std::to_underlying(PduType::CreateTab);
    }
    [[nodiscard]] constexpr uint64_t tagOf(SplitPane const&) noexcept
    {
        return std::to_underlying(PduType::SplitPane);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ClosePane const&) noexcept
    {
        return std::to_underlying(PduType::ClosePane);
    }
    [[nodiscard]] constexpr uint64_t tagOf(NewWindow const&) noexcept
    {
        return std::to_underlying(PduType::NewWindow);
    }

    // --- body encoders ------------------------------------------------------

    void encodeBody(Writer&, Invalid const&)
    {
    } // an unknown PDU has an opaque (empty) body

    void encodeBody(Writer& out, ClientHello const& pdu)
    {
        out.u32(pdu.codecVersion);
        out.string(pdu.token);
    }
    void encodeBody(Writer& out, ServerHello const& pdu)
    {
        out.u32(pdu.codecVersion);
    }

    void encodeBody(Writer& out, Input const& pdu)
    {
        out.varint(pdu.session);
        out.blob(pdu.data);
    }

    void encodeBody(Writer& out, ResizeRequest const& pdu)
    {
        out.varint(pdu.columns);
        out.varint(pdu.lines);
    }

    void encodeBody(Writer& out, FetchImage const& pdu)
    {
        out.varint(pdu.session);
        out.u32(pdu.imageId);
    }

    void encodeBody(Writer& out, ImageData const& pdu)
    {
        out.u32(pdu.imageId);
        out.u8(pdu.format);
        out.u32(pdu.width);
        out.u32(pdu.height);
        out.blob(pdu.data);
    }

    void encodeBody(Writer& out, ImageGone const& pdu)
    {
        out.u32(pdu.imageId);
    }

    void encodeBody(Writer& out, SessionState const& pdu)
    {
        out.varint(pdu.session);
        out.varint(pdu.columns);
        out.varint(pdu.lines);
        out.u8(pdu.screenType);
        out.svarint(pdu.cursorLine);
        out.svarint(pdu.cursorColumn);
        out.u8(pdu.cursorShape);
        out.u8(pdu.cursorVisible);
        out.string(pdu.title);
        out.u32(pdu.defaultForeground);
        out.u32(pdu.defaultBackground);
        out.varint(pdu.palette.size());
        for (auto const color: pdu.palette)
            out.u32(color);
        out.string(pdu.cwd);
        out.u8(pdu.statusDisplayType);
        out.u8(pdu.activeStatusDisplay);
        out.u8(pdu.kittyKeyboardFlags);
    }

    void encodeCell(Writer& out, WireCell const& cell)
    {
        out.varint(cell.codepoint);
        out.varint(cell.clusterExtras.size());
        for (auto const codepoint: cell.clusterExtras)
            out.varint(codepoint);
        out.u8(cell.width);
        out.u8(cell.scale);
        out.u16(cell.textScaleExtras);
        out.u16(cell.hyperlink);
        out.u32(cell.foreground);
        out.u32(cell.background);
        out.u32(cell.underlineColor);
        out.u32(cell.flags);
    }

    void encodeLine(Writer& out, WireLine const& line)
    {
        out.svarint(line.stableId);
        out.u16(line.flags);
        out.varint(line.columns);
        out.varint(line.cells.size());
        for (auto const& cell: line.cells)
            encodeCell(out, cell);
        out.u32(line.fillForeground);
        out.u32(line.fillBackground);
    }

    void encodeBody(Writer& out, Delta const& pdu)
    {
        out.varint(pdu.session);
        out.varint(pdu.generation);
        out.varint(pdu.seqno);
        out.u8(pdu.snapshot);
        out.svarint(pdu.stableViewportBase);
        out.svarint(pdu.stableFloor);
        out.svarint(pdu.cursorLine);
        out.svarint(pdu.cursorColumn);

        out.varint(pdu.lines.size());
        for (auto const& line: pdu.lines)
            encodeLine(out, line);

        out.varint(pdu.hyperlinks.size());
        for (auto const& entry: pdu.hyperlinks)
        {
            out.u16(entry.id);
            out.string(entry.uri);
        }

        out.varint(pdu.imageCells.size());
        for (auto const& entry: pdu.imageCells)
        {
            out.svarint(entry.stableId);
            out.u16(entry.column);
            out.u32(entry.imageId);
            out.u16(entry.offsetLine);
            out.u16(entry.offsetColumn);
            out.u8(entry.layer);
        }

        out.varint(pdu.setModes.size());
        for (auto const mode: pdu.setModes)
            out.varint(mode);

        out.u8(pdu.titleChanged);
        out.string(pdu.title);
        out.u8(pdu.cursorShapeChanged);
        out.u8(pdu.cursorShape);
        out.u8(pdu.cwdChanged);
        out.string(pdu.cwd);
        out.u8(pdu.colorsChanged);
        out.u32(pdu.defaultForeground);
        out.u32(pdu.defaultBackground);
        out.u8(pdu.statusChanged);
        out.u8(pdu.statusDisplayType);
        out.u8(pdu.activeStatusDisplay);
        out.u8(pdu.statusLinesChanged);
        out.varint(pdu.statusLines.size());
        for (auto const& line: pdu.statusLines)
            encodeLine(out, line);
        out.u8(pdu.kittyKeyboardChanged);
        out.u8(pdu.kittyKeyboardFlags);
    }

    void encodeBody(Writer& out, SessionEvent const& pdu)
    {
        out.varint(pdu.session);
        out.u8(pdu.kind);
        out.string(pdu.a);
        out.string(pdu.b);
    }

    void encodeBody(Writer&, CreateTab const&)
    {
    }

    void encodeBody(Writer&, NewWindow const&)
    {
    }

    void encodeBody(Writer& out, SplitPane const& pdu)
    {
        out.varint(pdu.session);
        out.u8(pdu.orientation);
        out.u16(pdu.ratio);
    }

    void encodeBody(Writer& out, ClosePane const& pdu)
    {
        out.varint(pdu.session);
    }

    /// Encodes one split-tree node pre-order (recurses into its children).
    void encodePane(Writer& out, WirePane const& pane)
    {
        out.varint(pane.paneId);
        out.u8(pane.split);
        out.varint(pane.session);
        out.u16(pane.ratio);
        out.varint(pane.children.size());
        for (auto const& child: pane.children)
            encodePane(out, child);
    }

    void encodeBody(Writer& out, LayoutState const& pdu)
    {
        out.varint(pdu.window);
        out.u32(pdu.activeTab);
        out.varint(pdu.tabs.size());
        for (auto const& tab: pdu.tabs)
        {
            out.varint(tab.tabId);
            out.varint(tab.activePane);
            out.varint(tab.zoomedPane);
            out.string(tab.title);
            out.u8(tab.hasColor);
            out.u32(tab.color);
            encodePane(out, tab.root);
        }
    }

    // --- body decoders (one table row each) ---------------------------------

    using DecodeResult = std::expected<DecodedPdu, DecodeError>;

    DecodeResult decodeClientHello(Reader& in)
    {
        auto pdu = ClientHello {};
        auto error = DecodeError {};
        if (!assign(in.u32(), pdu.codecVersion, error) || !assign(in.string(), pdu.token, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeServerHello(Reader& in)
    {
        auto pdu = ServerHello {};
        auto error = DecodeError {};
        if (!assign(in.u32(), pdu.codecVersion, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeInput(Reader& in)
    {
        auto pdu = Input {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error))
            return std::unexpected(error);
        auto const data = in.blob();
        if (!data)
            return std::unexpected(data.error());
        pdu.data.assign(data->begin(), data->end());
        return pdu;
    }

    DecodeResult decodeResizeRequest(Reader& in)
    {
        auto pdu = ResizeRequest {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.columns, error) || !assign(in.varint(), pdu.lines, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeFetchImage(Reader& in)
    {
        auto pdu = FetchImage {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.u32(), pdu.imageId, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeImageData(Reader& in)
    {
        auto pdu = ImageData {};
        auto error = DecodeError {};
        if (!assign(in.u32(), pdu.imageId, error) || !assign(in.u8(), pdu.format, error)
            || !assign(in.u32(), pdu.width, error) || !assign(in.u32(), pdu.height, error))
            return std::unexpected(error);
        auto const data = in.blob();
        if (!data)
            return std::unexpected(data.error());
        pdu.data.assign(data->begin(), data->end());
        return pdu;
    }

    DecodeResult decodeImageGone(Reader& in)
    {
        auto pdu = ImageGone {};
        auto error = DecodeError {};
        if (!assign(in.u32(), pdu.imageId, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeSessionState(Reader& in)
    {
        auto pdu = SessionState {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.varint(), pdu.columns, error)
            || !assign(in.varint(), pdu.lines, error) || !assign(in.u8(), pdu.screenType, error)
            || !assign(in.svarint(), pdu.cursorLine, error) || !assign(in.svarint(), pdu.cursorColumn, error)
            || !assign(in.u8(), pdu.cursorShape, error) || !assign(in.u8(), pdu.cursorVisible, error)
            || !assign(in.string(), pdu.title, error))
            return std::unexpected(error);
        if (!assign(in.u32(), pdu.defaultForeground, error)
            || !assign(in.u32(), pdu.defaultBackground, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.palette, [](Reader& reader) { return reader.u32(); });
            !decoded)
            return std::unexpected(decoded.error());
        if (!assign(in.string(), pdu.cwd, error) || !assign(in.u8(), pdu.statusDisplayType, error)
            || !assign(in.u8(), pdu.activeStatusDisplay, error)
            || !assign(in.u8(), pdu.kittyKeyboardFlags, error))
            return std::unexpected(error);
        return pdu;
    }

    [[nodiscard]] std::expected<WireCell, DecodeError> decodeCell(Reader& in)
    {
        auto cell = WireCell {};
        auto error = DecodeError {};
        if (!assign(in.varint(), cell.codepoint, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in,
                                              cell.clusterExtras,
                                              [](Reader& reader) {
                                                  return reader.varint().transform(
                                                      [](uint64_t v) { return static_cast<char32_t>(v); });
                                              });
            !decoded)
            return std::unexpected(decoded.error());
        if (!assign(in.u8(), cell.width, error) || !assign(in.u8(), cell.scale, error)
            || !assign(in.u16(), cell.textScaleExtras, error) || !assign(in.u16(), cell.hyperlink, error)
            || !assign(in.u32(), cell.foreground, error) || !assign(in.u32(), cell.background, error)
            || !assign(in.u32(), cell.underlineColor, error) || !assign(in.u32(), cell.flags, error))
            return std::unexpected(error);
        return cell;
    }

    [[nodiscard]] std::expected<WireLine, DecodeError> decodeLine(Reader& in)
    {
        auto line = WireLine {};
        auto error = DecodeError {};
        if (!assign(in.svarint(), line.stableId, error) || !assign(in.u16(), line.flags, error)
            || !assign(in.varint(), line.columns, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, line.cells, decodeCell); !decoded)
            return std::unexpected(decoded.error());
        if (!assign(in.u32(), line.fillForeground, error) || !assign(in.u32(), line.fillBackground, error))
            return std::unexpected(error);
        return line;
    }

    DecodeResult decodeDelta(Reader& in)
    {
        auto pdu = Delta {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.varint(), pdu.generation, error)
            || !assign(in.varint(), pdu.seqno, error) || !assign(in.u8(), pdu.snapshot, error)
            || !assign(in.svarint(), pdu.stableViewportBase, error)
            || !assign(in.svarint(), pdu.stableFloor, error) || !assign(in.svarint(), pdu.cursorLine, error)
            || !assign(in.svarint(), pdu.cursorColumn, error))
            return std::unexpected(error);

        if (auto const decoded = decodeVector(in, pdu.lines, decodeLine); !decoded)
            return std::unexpected(decoded.error());

        if (auto const decoded = decodeVector(
                in,
                pdu.hyperlinks,
                [](Reader& reader) -> std::expected<HyperlinkEntry, DecodeError> {
                    auto entry = HyperlinkEntry {};
                    auto error = DecodeError {};
                    if (!assign(reader.u16(), entry.id, error) || !assign(reader.string(), entry.uri, error))
                        return std::unexpected(error);
                    return entry;
                });
            !decoded)
            return std::unexpected(decoded.error());

        if (auto const decoded =
                decodeVector(in,
                             pdu.imageCells,
                             [](Reader& reader) -> std::expected<ImageCellEntry, DecodeError> {
                                 auto entry = ImageCellEntry {};
                                 auto error = DecodeError {};
                                 if (!assign(reader.svarint(), entry.stableId, error)
                                     || !assign(reader.u16(), entry.column, error)
                                     || !assign(reader.u32(), entry.imageId, error)
                                     || !assign(reader.u16(), entry.offsetLine, error)
                                     || !assign(reader.u16(), entry.offsetColumn, error)
                                     || !assign(reader.u8(), entry.layer, error))
                                     return std::unexpected(error);
                                 return entry;
                             });
            !decoded)
            return std::unexpected(decoded.error());

        if (auto const decoded = decodeVector(in,
                                              pdu.setModes,
                                              [](Reader& reader) {
                                                  return reader.varint().transform(
                                                      [](uint64_t v) { return static_cast<uint32_t>(v); });
                                              });
            !decoded)
            return std::unexpected(decoded.error());

        if (!assign(in.u8(), pdu.titleChanged, error) || !assign(in.string(), pdu.title, error)
            || !assign(in.u8(), pdu.cursorShapeChanged, error) || !assign(in.u8(), pdu.cursorShape, error)
            || !assign(in.u8(), pdu.cwdChanged, error) || !assign(in.string(), pdu.cwd, error)
            || !assign(in.u8(), pdu.colorsChanged, error) || !assign(in.u32(), pdu.defaultForeground, error)
            || !assign(in.u32(), pdu.defaultBackground, error) || !assign(in.u8(), pdu.statusChanged, error)
            || !assign(in.u8(), pdu.statusDisplayType, error)
            || !assign(in.u8(), pdu.activeStatusDisplay, error)
            || !assign(in.u8(), pdu.statusLinesChanged, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.statusLines, decodeLine); !decoded)
            return std::unexpected(decoded.error());
        if (!assign(in.u8(), pdu.kittyKeyboardChanged, error)
            || !assign(in.u8(), pdu.kittyKeyboardFlags, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeSessionEvent(Reader& in)
    {
        auto pdu = SessionEvent {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.u8(), pdu.kind, error)
            || !assign(in.string(), pdu.a, error) || !assign(in.string(), pdu.b, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeCreateTab(Reader&)
    {
        return CreateTab {};
    }

    DecodeResult decodeNewWindow(Reader&)
    {
        return NewWindow {};
    }

    DecodeResult decodeSplitPane(Reader& in)
    {
        auto pdu = SplitPane {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.u8(), pdu.orientation, error)
            || !assign(in.u16(), pdu.ratio, error))
            return std::unexpected(error);
        // The wire orientation is a vtworkspace::SplitState value: exactly 1 (Horizontal)
        // or 2 (Vertical) — rejected here, at the protocol boundary, exactly as
        // decodePane rejects an out-of-range WirePane.split. Fail closed on any
        // other byte so no invalid SplitState can ever reach the layout tree (a
        // "None" split with children renders as a leaf; anything else re-serializes
        // as garbage every client rejects).
        if (pdu.orientation != 1 && pdu.orientation != 2)
            return std::unexpected(DecodeError::MalformedPdu);
        return pdu;
    }

    DecodeResult decodeClosePane(Reader& in)
    {
        auto pdu = ClosePane {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error))
            return std::unexpected(error);
        return pdu;
    }

    /// Decodes one split-tree node (recursing into its children). @p depth bounds
    /// the recursion so a hostile deeply-nested tree cannot overflow the stack.
    std::expected<WirePane, DecodeError> decodePane(Reader& in, int depth)
    {
        if (depth <= 0)
            return std::unexpected(DecodeError::MalformedPdu);
        auto pane = WirePane {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pane.paneId, error) || !assign(in.u8(), pane.split, error)
            || !assign(in.varint(), pane.session, error) || !assign(in.u16(), pane.ratio, error))
            return std::unexpected(error);
        auto childCount = uint64_t { 0 };
        if (!assign(in.varint(), childCount, error))
            return std::unexpected(error);
        // A pane node's split state and child count must agree (SplitState in
        // vtworkspace/Primitives.h): split 0 (None) is a leaf with no children; split 1/2
        // (Horizontal/Vertical) is a binary split with exactly two. Reject any other
        // combination — an out-of-range split, or a split lacking its two children —
        // so the layout converters (wireToLayoutPane/mapLeaves) can index
        // children[0]/[1] unconditionally instead of reading out of bounds on a
        // hostile PDU.
        if (pane.split > 2 || childCount != (pane.split == 0 ? uint64_t { 0 } : uint64_t { 2 }))
            return std::unexpected(DecodeError::MalformedPdu);
        for ([[maybe_unused]] auto const _: std::views::iota(uint64_t { 0 }, childCount))
        {
            auto child = decodePane(in, depth - 1);
            if (!child)
                return std::unexpected(child.error());
            pane.children.push_back(std::move(*child));
        }
        return pane;
    }

    DecodeResult decodeLayoutState(Reader& in)
    {
        auto pdu = LayoutState {};
        auto error = DecodeError {};
        auto tabCount = uint64_t { 0 };
        if (!assign(in.varint(), pdu.window, error) || !assign(in.u32(), pdu.activeTab, error)
            || !assign(in.varint(), tabCount, error))
            return std::unexpected(error);
        // tmux's WINDOW_MAXIMUM is 10000; a count far beyond that is a lie (the
        // frame-size bound already caps real payloads, but reject early).
        if (tabCount > 100000)
            return std::unexpected(DecodeError::MalformedPdu);
        for ([[maybe_unused]] auto const _: std::views::iota(uint64_t { 0 }, tabCount))
        {
            auto tab = WireTab {};
            if (!assign(in.varint(), tab.tabId, error) || !assign(in.varint(), tab.activePane, error)
                || !assign(in.varint(), tab.zoomedPane, error) || !assign(in.string(), tab.title, error)
                || !assign(in.u8(), tab.hasColor, error) || !assign(in.u32(), tab.color, error))
                return std::unexpected(error);
            auto root = decodePane(in, /*depth=*/256);
            if (!root)
                return std::unexpected(root.error());
            tab.root = std::move(*root);
            pdu.tabs.push_back(std::move(tab));
        }
        return pdu;
    }

    /// The decode half of the catalog: one row per known tag.
    struct DecodeRow
    {
        PduType tag;
        DecodeResult (*decode)(Reader&);
    };

    constexpr auto DecodeTable = std::array {
        DecodeRow { PduType::ClientHello, decodeClientHello },
        DecodeRow { PduType::ServerHello, decodeServerHello },
        DecodeRow { PduType::Input, decodeInput },
        DecodeRow { PduType::ResizeRequest, decodeResizeRequest },
        DecodeRow { PduType::FetchImage, decodeFetchImage },
        DecodeRow { PduType::ImageData, decodeImageData },
        DecodeRow { PduType::ImageGone, decodeImageGone },
        DecodeRow { PduType::SessionState, decodeSessionState },
        DecodeRow { PduType::Delta, decodeDelta },
        DecodeRow { PduType::SessionEvent, decodeSessionEvent },
        DecodeRow { PduType::LayoutState, decodeLayoutState },
        DecodeRow { PduType::CreateTab, decodeCreateTab },
        DecodeRow { PduType::SplitPane, decodeSplitPane },
        DecodeRow { PduType::ClosePane, decodeClosePane },
        DecodeRow { PduType::NewWindow, decodeNewWindow },
    };
} // namespace

void encodePdu(Writer& sink, uint64_t serial, DecodedPdu const& pdu)
{
    auto body = Writer {};
    auto const ident = std::visit(
        [&body](auto const& alternative) {
            encodeBody(body, alternative);
            return tagOf(alternative);
        },
        pdu);
    writeFrame(sink, serial, ident, body.view());
}

std::expected<DecodedFrame, DecodeError> decodePdu(std::span<std::byte const> data)
{
    auto const frame = readFrame(data);
    if (!frame)
        return std::unexpected(frame.error());

    auto reader = Reader { frame->body };
    auto pdu = [&]() -> DecodeResult {
        for (auto const& row: DecodeTable)
            if (std::to_underlying(row.tag) == frame->ident)
                return row.decode(reader);
        // Unknown idents are data, not errors: newer peers keep talking to us.
        return DecodedPdu { Invalid { .ident = frame->ident } };
    }();
    if (!pdu)
        // readFrame already proved the whole frame is buffered, so a body that
        // runs short mid-value is a malformed PDU, never "read more from the
        // socket" — folding it here stops the pump retrying a complete frame
        // forever (and its buffer growing without bound) on a lying peer.
        return std::unexpected(pdu.error() == DecodeError::NeedMoreData ? DecodeError::MalformedPdu
                                                                        : pdu.error());

    if (!std::holds_alternative<Invalid>(*pdu) && reader.remaining() != 0)
        return std::unexpected(DecodeError::TrailingBytes);

    return DecodedFrame {
        .serial = frame->serial,
        .pdu = std::move(*pdu),
        .consumed = frame->consumed,
    };
}

} // namespace muxserver::proto
