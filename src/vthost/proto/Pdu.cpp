// SPDX-License-Identifier: Apache-2.0
#include <vthost/proto/Pdu.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace vthost::proto
{

namespace
{
    /// An integer type `std::in_range` accepts: `bool` and the character types are integral but
    /// not *standard* integers, and passing either to it is a hard error rather than a false.
    template <typename T>
    concept StandardInteger =
        std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>
        && !std::same_as<std::remove_cv_t<T>, char> && !std::same_as<std::remove_cv_t<T>, wchar_t>
        && !std::same_as<std::remove_cv_t<T>, char8_t> && !std::same_as<std::remove_cv_t<T>, char16_t>
        && !std::same_as<std::remove_cv_t<T>, char32_t>;

    /// Assigns a checked read into @p out, or records the error.
    ///
    /// A read that does not FIT @p out is a protocol error, not something to truncate. Every
    /// integral field on this wire is carried by a varint wide enough to hold more than the field
    /// does, so a plain `static_cast` would quietly turn a nonsense value into a plausible one:
    /// a peer naming 2^32 + 80 columns arrives as 80, sailing past any range check downstream
    /// because by then the evidence is gone. Range-checking here means a value can be trusted to
    /// mean what it says everywhere else.
    ///
    /// Non-integral reads (strings, byte vectors) pass through unchecked — there is nothing to
    /// narrow — as do `bool` and the character types, which `std::in_range` does not accept.
    template <typename T, typename U>
    [[nodiscard]] bool assign(std::expected<U, DecodeError> value, T& out, DecodeError& error)
    {
        if (!value)
        {
            error = value.error();
            return false;
        }
        if constexpr (StandardInteger<T> && StandardInteger<U>)
        {
            if (!std::in_range<T>(*value))
            {
                error = DecodeError::MalformedPdu;
                return false;
            }
            out = static_cast<T>(*value);
        }
        else
            out = static_cast<T>(std::move(*value));
        return true;
    }

    /// Narrows a varint read to a codepoint, rejecting anything that is not a Unicode scalar value.
    ///
    /// `assign` above cannot do this one: `std::in_range` rejects the character types, so char32_t
    /// is excluded from StandardInteger and falls into the unchecked branch — and char32_t's own
    /// range would be the wrong question anyway. A codepoint's range is Unicode's, not 32 bits'.
    /// @param value The varint just read.
    /// @return The codepoint, or the reason it is not one.
    [[nodiscard]] std::expected<char32_t, DecodeError> asCodepoint(std::expected<uint64_t, DecodeError> value)
    {
        return value.and_then([](uint64_t raw) -> std::expected<char32_t, DecodeError> {
            if (raw > MaxCodepoint)
                return std::unexpected(DecodeError::MalformedPdu);
            return static_cast<char32_t>(raw);
        });
    }

    /// Reserves room for @p count elements, but never more MEMORY than the reader's remaining bytes
    /// could possibly back (@see boundedReserveCount, which states the bound and is tested against
    /// the element types that actually travel).
    ///
    /// A lie in @p count therefore cannot trigger a giant allocation (bad_alloc/length_error
    /// escaping the std::expected path); the decode loop still runs to @p count and trips
    /// MalformedPdu on the short body instead.
    template <typename T>
    void reserveBounded(std::vector<T>& out, std::size_t count, Reader const& in)
    {
        out.reserve(boundedReserveCount<T>(count, in.remaining()));
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
    [[nodiscard]] constexpr uint64_t tagOf(ResizePane const&) noexcept
    {
        return std::to_underlying(PduType::ResizePane);
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
    [[nodiscard]] constexpr uint64_t tagOf(SessionBell const&) noexcept
    {
        return std::to_underlying(PduType::SessionBell);
    }
    [[nodiscard]] constexpr uint64_t tagOf(SessionNotify const&) noexcept
    {
        return std::to_underlying(PduType::SessionNotify);
    }
    [[nodiscard]] constexpr uint64_t tagOf(SessionClipboard const&) noexcept
    {
        return std::to_underlying(PduType::SessionClipboard);
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
    [[nodiscard]] constexpr uint64_t tagOf(ResizeSplit const&) noexcept
    {
        return std::to_underlying(PduType::ResizeSplit);
    }

    // --- body encoders ------------------------------------------------------

    void encodeBody(Writer&, Invalid const&)
    {
    } // an unknown PDU has an opaque (empty) body

    /// SessionState and Delta both carry it — once as the state outright, once behind a changed
    /// flag — so the field order lives here rather than being written out twice.
    void encodeMouseState(Writer& out, MouseState const& mouse)
    {
        out.u16(mouse.protocol);
        out.u8(mouse.transport);
        out.u8(mouse.wheelMode);
    }

    /// The inverse of encodeMouseState. Values are carried through unjudged; deciding what an
    /// out-of-range one means belongs to the side that applies it (@see vthost/MouseWire.h), not to
    /// the codec, exactly as SplitPane's ratio is.
    [[nodiscard]] bool decodeMouseState(Reader& in, MouseState& mouse, DecodeError& error)
    {
        return assign(in.u16(), mouse.protocol, error) && assign(in.u8(), mouse.transport, error)
               && assign(in.u8(), mouse.wheelMode, error);
    }

    void encodeSessionSettings(Writer& out, WireSessionSettings const& settings)
    {
        out.svarint(settings.historyLineCount);
        out.u8(settings.terminalId);
        out.u8(settings.graphemeClustering);
        out.u8(settings.allowReflowOnResize);
        out.u32(settings.maxImageRegisterCount);
        out.string(settings.wordDelimiters);
        out.varint(settings.frozenModes.size());
        for (auto const& frozen: settings.frozenModes)
        {
            out.varint(frozen.mode);
            out.u8(frozen.frozenAs);
        }
    }

    void encodeBody(Writer& out, ClientHello const& pdu)
    {
        out.u32(pdu.codecVersion);
        out.string(pdu.token);
        // A presence byte, so a client with no preference of its own costs exactly one byte rather
        // than a block of wire defaults the server would have to tell apart from real values.
        out.u8(pdu.sessionSettings.has_value() ? 1 : 0);
        if (pdu.sessionSettings)
            encodeSessionSettings(out, *pdu.sessionSettings);
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

    void encodeBody(Writer& out, ResizePane const& pdu)
    {
        out.varint(pdu.session);
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

    /// The optional string fields of a context, in WIRE ORDER, with the presence bit each is gated on.
    ///
    /// ONE table, walked by both @ref encodeContext and @ref decodeContext, so the two cannot disagree
    /// about the order. Spelled out as two hand-written sequences they agreed only by coincidence, and
    /// a disagreement does not fail: it desynchronizes the stream mid-record and surfaces as garbage in
    /// some later, unrelated field.
    ///
    /// The bit numbers are the PROTOCOL's and are pinned HERE rather than read from vtbackend, for the
    /// reason GridWire.cpp pins LineFlag and CellFlag: they travel on the wire verbatim inside
    /// `present`, so renumbering vtbackend's own field table -- which that table's comment actively
    /// invites -- must not silently change what an older peer decodes. vthost/ContextWire.hpp
    /// static_asserts that the two still agree.
    constexpr auto ContextStringFields = std::array {
        std::pair { uint16_t { 1 << 1 }, &WireContext::user },
        std::pair { uint16_t { 1 << 2 }, &WireContext::hostname },
        std::pair { uint16_t { 1 << 3 }, &WireContext::machineId },
        std::pair { uint16_t { 1 << 4 }, &WireContext::bootId },
        std::pair { uint16_t { 1 << 7 }, &WireContext::comm },
        std::pair { uint16_t { 1 << 8 }, &WireContext::workingDirectory },
        std::pair { uint16_t { 1 << 9 }, &WireContext::commandLine },
        std::pair { uint16_t { 1 << 10 }, &WireContext::vm },
        std::pair { uint16_t { 1 << 11 }, &WireContext::container },
        std::pair { uint16_t { 1 << 12 }, &WireContext::targetUser },
        std::pair { uint16_t { 1 << 13 }, &WireContext::targetHost },
        std::pair { uint16_t { 1 << 14 }, &WireContext::sessionId },
    };

    /// The optional varint fields, same rules. @see ContextStringFields.
    constexpr auto ContextVarintFields = std::array {
        std::pair { uint16_t { 1 << 5 }, &WireContext::pid },
        std::pair { uint16_t { 1 << 6 }, &WireContext::pidFdId },
    };

    /// Encodes one context record, writing only the fields its `present` mask names.
    ///
    /// The mask is written FIRST so the decoder knows what follows without a per-field presence byte:
    /// a systemd `type=command` record then costs six strings on the wire rather than fifteen empty
    /// length prefixes.
    void encodeContext(Writer& out, WireContext const& context)
    {
        out.varint(context.id);
        out.varint(context.parent);
        out.string(context.identifier);
        out.u8(context.type);
        out.u16(context.present);

        for (auto const& [bit, member]: ContextStringFields)
            if (context.present & bit)
                out.string(context.*member);
        for (auto const& [bit, member]: ContextVarintFields)
            if (context.present & bit)
                out.varint(context.*member);

        out.u8(context.exitKind);
        out.u8(context.signal);
        out.varint(context.status);
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
        out.u8(pdu.modifyOtherKeys);
        encodeMouseState(out, pdu.mouse);
        out.u8(pdu.progressState);
        out.u8(pdu.progressPercentage);
        out.varint(pdu.contexts.size());
        for (auto const& context: pdu.contexts)
            encodeContext(out, context);
        out.varint(pdu.contextChain.size());
        for (auto const id: pdu.contextChain)
            out.u16(id);
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
        out.u16(line.contextId);
        out.svarint(line.promptEndOffset);
        out.svarint(line.commandEndOffset);
        out.varint(line.columns);
        out.varint(line.cells.size());
        for (auto const& cell: line.cells)
            encodeCell(out, cell);
        out.u32(line.fillForeground);
        out.u32(line.fillBackground);
        out.u32(line.fillUnderlineColor);
        out.u32(line.fillFlags);
    }

    void encodeBody(Writer& out, Delta const& pdu)
    {
        out.varint(pdu.session);
        out.varint(pdu.generation);
        out.varint(pdu.seqno);
        out.u8(pdu.snapshot);
        out.u8(pdu.snapshotPart);
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
            out.u8(entry.alignment);
            out.u8(entry.resize);
        }

        out.varint(pdu.setModes.size());
        for (auto const mode: pdu.setModes)
            out.varint(mode);

        out.varint(pdu.setAnsiModes.size());
        for (auto const mode: pdu.setAnsiModes)
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
        out.u8(pdu.modifyOtherKeysChanged);
        out.u8(pdu.modifyOtherKeys);
        out.u8(pdu.mouseChanged);
        encodeMouseState(out, pdu.mouse);
        out.u8(pdu.progressChanged);
        out.u8(pdu.progressState);
        out.u8(pdu.progressPercentage);
        out.u8(pdu.contextChanged);
        out.u16(pdu.activeContext);
        out.varint(pdu.contexts.size());
        for (auto const& context: pdu.contexts)
            encodeContext(out, context);
    }

    void encodeBody(Writer& out, SessionBell const& pdu)
    {
        out.varint(pdu.session);
    }

    void encodeBody(Writer& out, SessionNotify const& pdu)
    {
        out.varint(pdu.session);
        out.string(pdu.title);
        out.string(pdu.body);
    }

    void encodeBody(Writer& out, SessionClipboard const& pdu)
    {
        out.varint(pdu.session);
        out.string(pdu.selection);
        out.string(pdu.data);
    }

    void encodeBody(Writer& out, CreateTab const& pdu)
    {
        out.varint(pdu.session);
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

    void encodeBody(Writer& out, ResizeSplit const& pdu)
    {
        out.varint(pdu.firstSession);
        out.varint(pdu.secondSession);
        out.u16(pdu.ratio);
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

    [[nodiscard]] std::expected<WireSessionSettings, DecodeError> decodeSessionSettings(Reader& in)
    {
        auto settings = WireSessionSettings {};
        auto error = DecodeError {};
        if (!assign(in.svarint(), settings.historyLineCount, error)
            || !assign(in.u8(), settings.terminalId, error)
            || !assign(in.u8(), settings.graphemeClustering, error)
            || !assign(in.u8(), settings.allowReflowOnResize, error)
            || !assign(in.u32(), settings.maxImageRegisterCount, error)
            || !assign(in.string(), settings.wordDelimiters, error))
            return std::unexpected(error);
        if (auto const decoded =
                decodeVector(in,
                             settings.frozenModes,
                             [](Reader& reader) -> std::expected<WireFrozenMode, DecodeError> {
                                 auto frozen = WireFrozenMode {};
                                 auto error = DecodeError {};
                                 if (!assign(reader.varint(), frozen.mode, error)
                                     || !assign(reader.u8(), frozen.frozenAs, error))
                                     return std::unexpected(error);
                                 return frozen;
                             });
            !decoded)
            return std::unexpected(decoded.error());
        // Range-checking the VALUES is the server's job, not the codec's: an out-of-range setting is
        // a preference this daemon declines, not a malformed frame. @see fromWireSessionSettings.
        return settings;
    }

    DecodeResult decodeClientHello(Reader& in)
    {
        auto pdu = ClientHello {};
        auto error = DecodeError {};
        if (!assign(in.u32(), pdu.codecVersion, error) || !assign(in.string(), pdu.token, error))
            return std::unexpected(error);
        auto present = uint8_t {};
        if (!assign(in.u8(), present, error))
            return std::unexpected(error);
        if (present != 0)
        {
            auto settings = decodeSessionSettings(in);
            if (!settings)
                return std::unexpected(settings.error());
            pdu.sessionSettings = *std::move(settings);
        }
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

    DecodeResult decodeResizePane(Reader& in)
    {
        auto pdu = ResizePane {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.varint(), pdu.columns, error)
            || !assign(in.varint(), pdu.lines, error))
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

    /// Decodes one context record, reading only the fields its `present` mask names.
    ///
    /// Walks the SAME tables the encoder writes, so the read order is structurally identical rather
    /// than maintained in parallel by hand. Enum-ish bytes are carried through RAW and validated by the
    /// consumer (@see vthost::fromWireContext), which is what keeps this codec free of vtbackend: only
    /// the presence mask is validated here, and it has to be, because the reads below are driven by it.
    [[nodiscard]] std::expected<WireContext, DecodeError> decodeContext(Reader& in)
    {
        auto context = WireContext {};
        auto error = DecodeError {};
        if (!assign(in.varint(), context.id, error) || !assign(in.varint(), context.parent, error)
            || !assign(in.string(), context.identifier, error) || !assign(in.u8(), context.type, error)
            || !assign(in.u16(), context.present, error))
            return std::unexpected(error);

        // Any bit this build does not assign is cleared, so the reads below cannot go looking for a
        // field there is no member for and desynchronize the stream. Masked rather than rejected: an
        // unknown bit is an unknown FIELD, and the protocol's rule for one of those is to ignore it and
        // keep the rest, so a peer built from a newer commit degrades to the fields this build knows.
        context.present = static_cast<uint16_t>(context.present & ContextPresentMask);

        for (auto const& [bit, member]: ContextStringFields)
            if ((context.present & bit) && !assign(in.string(), context.*member, error))
                return std::unexpected(error);
        for (auto const& [bit, member]: ContextVarintFields)
            if ((context.present & bit) && !assign(in.varint(), context.*member, error))
                return std::unexpected(error);

        if (!assign(in.u8(), context.exitKind, error) || !assign(in.u8(), context.signal, error)
            || !assign(in.varint(), context.status, error))
            return std::unexpected(error);

        return context;
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
        // The announced grid is bounded here, at the protocol boundary, exactly as a client's
        // PROPOSED grid is. Note that `assign` narrows with a plain static_cast, so without this a
        // varint naming 2^40 columns would arrive as some arbitrary uint32_t — and the client sizes
        // a repaint buffer as lines * columns * 25.
        if (pdu.columns > MaxGridExtent || pdu.lines > MaxGridExtent)
            return std::unexpected(DecodeError::MalformedPdu);
        if (!assign(in.u32(), pdu.defaultForeground, error)
            || !assign(in.u32(), pdu.defaultBackground, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.palette, [](Reader& reader) { return reader.u32(); });
            !decoded)
            return std::unexpected(decoded.error());
        if (!assign(in.string(), pdu.cwd, error) || !assign(in.u8(), pdu.statusDisplayType, error)
            || !assign(in.u8(), pdu.activeStatusDisplay, error)
            || !assign(in.u8(), pdu.kittyKeyboardFlags, error) || !assign(in.u8(), pdu.modifyOtherKeys, error)
            || !decodeMouseState(in, pdu.mouse, error) || !assign(in.u8(), pdu.progressState, error)
            || !assign(in.u8(), pdu.progressPercentage, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.contexts, decodeContext); !decoded)
            return std::unexpected(decoded.error());
        if (auto const decoded =
                decodeVector(in, pdu.contextChain, [](Reader& reader) { return reader.u16(); });
            !decoded)
            return std::unexpected(decoded.error());
        return pdu;
    }

    [[nodiscard]] std::expected<WireCell, DecodeError> decodeCell(Reader& in)
    {
        auto cell = WireCell {};
        auto error = DecodeError {};
        auto const codepoint = asCodepoint(in.varint());
        if (!codepoint)
            return std::unexpected(codepoint.error());
        cell.codepoint = *codepoint;
        // The continuation codepoints of a grapheme cluster are held to the same range as the base
        // one: they reach the segmenter and the shaper through exactly the same path.
        if (auto const decoded = decodeVector(
                in, cell.clusterExtras, [](Reader& reader) { return asCodepoint(reader.varint()); });
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
            || !assign(in.u16(), line.contextId, error) || !assign(in.svarint(), line.promptEndOffset, error)
            || !assign(in.svarint(), line.commandEndOffset, error)
            || !assign(in.varint(), line.columns, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, line.cells, decodeCell); !decoded)
            return std::unexpected(decoded.error());
        if (!assign(in.u32(), line.fillForeground, error) || !assign(in.u32(), line.fillBackground, error)
            || !assign(in.u32(), line.fillUnderlineColor, error) || !assign(in.u32(), line.fillFlags, error))
            return std::unexpected(error);
        return line;
    }

    DecodeResult decodeDelta(Reader& in)
    {
        auto pdu = Delta {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.varint(), pdu.generation, error)
            || !assign(in.varint(), pdu.seqno, error) || !assign(in.u8(), pdu.snapshot, error)
            || !assign(in.u8(), pdu.snapshotPart, error)
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
                                     || !assign(reader.u8(), entry.layer, error)
                                     || !assign(reader.u8(), entry.alignment, error)
                                     || !assign(reader.u8(), entry.resize, error))
                                     return std::unexpected(error);
                                 return entry;
                             });
            !decoded)
            return std::unexpected(decoded.error());

        auto const decodeModeNumber = [](Reader& reader) {
            return reader.varint().transform([](uint64_t v) { return static_cast<uint32_t>(v); });
        };
        if (auto const decoded = decodeVector(in, pdu.setModes, decodeModeNumber); !decoded)
            return std::unexpected(decoded.error());
        if (auto const decoded = decodeVector(in, pdu.setAnsiModes, decodeModeNumber); !decoded)
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
            || !assign(in.u8(), pdu.kittyKeyboardFlags, error)
            || !assign(in.u8(), pdu.modifyOtherKeysChanged, error)
            || !assign(in.u8(), pdu.modifyOtherKeys, error) || !assign(in.u8(), pdu.mouseChanged, error)
            || !decodeMouseState(in, pdu.mouse, error) || !assign(in.u8(), pdu.progressChanged, error)
            || !assign(in.u8(), pdu.progressState, error) || !assign(in.u8(), pdu.progressPercentage, error)
            || !assign(in.u8(), pdu.contextChanged, error) || !assign(in.u16(), pdu.activeContext, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.contexts, decodeContext); !decoded)
            return std::unexpected(decoded.error());
        return pdu;
    }

    DecodeResult decodeSessionBell(Reader& in)
    {
        auto pdu = SessionBell {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeSessionNotify(Reader& in)
    {
        auto pdu = SessionNotify {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.string(), pdu.title, error)
            || !assign(in.string(), pdu.body, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeSessionClipboard(Reader& in)
    {
        auto pdu = SessionClipboard {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.string(), pdu.selection, error)
            || !assign(in.string(), pdu.data, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeCreateTab(Reader& in)
    {
        auto pdu = CreateTab {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error))
            return std::unexpected(error);
        return pdu;
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

    DecodeResult decodeResizeSplit(Reader& in)
    {
        auto pdu = ResizeSplit {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.firstSession, error) || !assign(in.varint(), pdu.secondSession, error)
            || !assign(in.u16(), pdu.ratio, error))
            return std::unexpected(error);
        // The ratio is carried, not judged — exactly as SplitPane's and WirePane's are. A value
        // outside (0, 1) is a nonsense LAYOUT, not a malformed frame, and fromWireRatio answers it
        // with an even split; rejecting the whole PDU here would be a stricter rule for one field
        // than its two twins live under.
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
        DecodeRow { PduType::ResizePane, decodeResizePane },
        DecodeRow { PduType::FetchImage, decodeFetchImage },
        DecodeRow { PduType::ImageData, decodeImageData },
        DecodeRow { PduType::ImageGone, decodeImageGone },
        DecodeRow { PduType::SessionState, decodeSessionState },
        DecodeRow { PduType::Delta, decodeDelta },
        DecodeRow { PduType::SessionBell, decodeSessionBell },
        DecodeRow { PduType::SessionNotify, decodeSessionNotify },
        DecodeRow { PduType::SessionClipboard, decodeSessionClipboard },
        DecodeRow { PduType::LayoutState, decodeLayoutState },
        DecodeRow { PduType::CreateTab, decodeCreateTab },
        DecodeRow { PduType::SplitPane, decodeSplitPane },
        DecodeRow { PduType::ClosePane, decodeClosePane },
        DecodeRow { PduType::NewWindow, decodeNewWindow },
        DecodeRow { PduType::ResizeSplit, decodeResizeSplit },
    };

    // The catalog and its decode half must stay in step. Invalid is the one alternative with
    // no row — it is what an unknown ident decodes TO (see the fallback in decodePdu) — hence
    // the +1. Adding a PDU without its decoder is a build break, not a silent passthrough.
    static_assert(DecodeTable.size() + 1 == std::variant_size_v<DecodedPdu>,
                  "every DecodedPdu alternative except Invalid needs a DecodeTable row");
} // namespace

std::size_t estimatedEncodedSize(WireLine const& line) noexcept
{
    // encodeCell writes a varint codepoint, a varint extras count, two u8, two u16 and four u32 —
    // 22 fixed bytes plus a 1..4 byte codepoint and whatever clusterExtras adds. encodeLine's own
    // header is two svarints, a u16, two varints and four u32. Both are rounded up to a round
    // number rather than computed exactly: @see the header for why an estimate is the point.
    constexpr auto BytesPerCell = std::size_t { 24 };
    constexpr auto BytesPerRowHeader = std::size_t { 24 };
    return BytesPerRowHeader + (line.cells.size() * BytesPerCell);
}

std::vector<SnapshotPiece> partitionSnapshotRows(std::span<WireLine const> lines, std::size_t budget)
{
    auto pieces = std::vector<SnapshotPiece> {};
    auto begin = std::size_t { 0 };
    while (begin < lines.size())
    {
        // The first row of a piece is taken unconditionally, which is what keeps an oversized row
        // from producing an empty span and looping forever.
        auto count = std::size_t { 1 };
        auto used = estimatedEncodedSize(lines[begin]);
        for (auto const& line: lines.subspan(begin + 1))
        {
            auto const cost = estimatedEncodedSize(line);
            if (used + cost > budget)
                break;
            used += cost;
            ++count;
        }
        pieces.push_back(SnapshotPiece { .begin = begin, .count = count });
        begin += count;
    }
    if (pieces.empty())
        pieces.push_back(SnapshotPiece {});

    // Marked in a second pass: how many pieces there are is only known once the partition is done,
    // and a lone piece must be Whole rather than a First whose Last never comes.
    if (pieces.size() > 1)
    {
        for (auto& piece: pieces)
            piece.part = SnapshotPart::Middle;
        pieces.front().part = SnapshotPart::First;
        pieces.back().part = SnapshotPart::Last;
    }
    return pieces;
}

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

std::optional<SessionEventPdu> asSessionEvent(DecodedPdu const& pdu)
{
    // One visit over the catalog rather than a chain of get_if at the call site: a fourth event
    // becomes an alternative of SessionEventPdu and this recognises it with no further edit.
    return std::visit(
        []<typename T>(T const& alternative) -> std::optional<SessionEventPdu> {
            if constexpr (std::is_constructible_v<SessionEventPdu, T>)
                return SessionEventPdu { alternative };
            else
                return std::nullopt;
        },
        pdu);
}

uint64_t sessionOf(SessionEventPdu const& event) noexcept
{
    return std::visit([](auto const& alternative) { return alternative.session; }, event);
}

PduType typeOf(DecodedPdu const& pdu) noexcept
{
    return std::visit(
        []<typename T>(T const& alternative) {
            // Invalid's tagOf yields the peer's off-catalog ident, which is not a PduType at
            // all — the alternative itself is what identifies it.
            if constexpr (std::is_same_v<T, Invalid>)
                return PduType::Invalid;
            else
                return static_cast<PduType>(tagOf(alternative));
        },
        pdu);
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

} // namespace vthost::proto
