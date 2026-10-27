// SPDX-License-Identifier: Apache-2.0
#include <vthost/proto/Pdu.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <ranges>
#include <span>
#include <tuple>
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
    [[nodiscard]] constexpr uint64_t tagOf(SessionSettingsChanged const&) noexcept
    {
        return std::to_underlying(PduType::SessionSettingsChanged);
    }
    [[nodiscard]] constexpr uint64_t tagOf(PerformOperation const&) noexcept
    {
        return std::to_underlying(PduType::PerformOperation);
    }
    [[nodiscard]] constexpr uint64_t tagOf(PermissionRequest const&) noexcept
    {
        return std::to_underlying(PduType::PermissionRequest);
    }
    [[nodiscard]] constexpr uint64_t tagOf(PermissionReply const&) noexcept
    {
        return std::to_underlying(PduType::PermissionReply);
    }
    [[nodiscard]] constexpr uint64_t tagOf(Unsupported const&) noexcept
    {
        return std::to_underlying(PduType::Unsupported);
    }
    [[nodiscard]] constexpr uint64_t tagOf(DaemonStatusRequest const&) noexcept
    {
        return std::to_underlying(PduType::DaemonStatusRequest);
    }
    [[nodiscard]] constexpr uint64_t tagOf(DaemonStatusReply const&) noexcept
    {
        return std::to_underlying(PduType::DaemonStatusReply);
    }
    [[nodiscard]] constexpr uint64_t tagOf(StopDaemon const&) noexcept
    {
        return std::to_underlying(PduType::StopDaemon);
    }
    [[nodiscard]] constexpr uint64_t tagOf(EditTab const&) noexcept
    {
        return std::to_underlying(PduType::EditTab);
    }
    [[nodiscard]] constexpr uint64_t tagOf(MergeTabIntoSplit const&) noexcept
    {
        return std::to_underlying(PduType::MergeTabIntoSplit);
    }
    [[nodiscard]] constexpr uint64_t tagOf(Heartbeat const&) noexcept
    {
        return std::to_underlying(PduType::Heartbeat);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ControlRequest const&) noexcept
    {
        return std::to_underlying(PduType::ControlRequest);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ControlReply const&) noexcept
    {
        return std::to_underlying(PduType::ControlReply);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ControlEvent const&) noexcept
    {
        return std::to_underlying(PduType::ControlEvent);
    }
    [[nodiscard]] constexpr uint64_t tagOf(ProgramStatus const&) noexcept
    {
        return std::to_underlying(PduType::ProgramStatus);
    }
    [[nodiscard]] constexpr uint64_t tagOf(SessionInputEcho const&) noexcept
    {
        return std::to_underlying(PduType::SessionInputEcho);
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
        out.u8(settings.sixelScrolling);
        out.string(settings.wordDelimiters);
        out.varint(settings.frozenModes.size());
        for (auto const& frozen: settings.frozenModes)
        {
            out.varint(frozen.mode);
            out.u8(frozen.frozenAs);
        }
    }

    void encodeOptionalSessionSettings(Writer& out, std::optional<WireSessionSettings> const& settings)
    {
        // A presence byte, so a client with no preference of its own costs exactly one byte rather
        // than a block of wire defaults the server would have to tell apart from real values.
        out.u8(settings.has_value() ? 1 : 0);
        if (settings)
            encodeSessionSettings(out, *settings);
    }

    void encodeCapabilities(Writer& out, CapabilitySet const& capabilities)
    {
        out.varint(capabilities.idents.size());
        for (auto const ident: capabilities.idents)
            out.varint(ident);
    }

    void encodeVersionRange(Writer& out, VersionRange versions)
    {
        out.varint(versions.min);
        out.varint(versions.max);
    }

    /// The marker, then the fields as one blob, then a zero byte.
    ///
    /// The blob and the zero are for the daemon of contour 0.7.0, which reads a hello whole before it
    /// checks the version, and refuses a byte too many or too few. It reads the blob as its token and the
    /// zero as "no session settings", so it reads this hello and answers with its version, where any other
    /// layout would have it drop the connection on a decode error. Codecs 2 and 3 read the marker as a
    /// version not their own and consume the rest unread. This shape is kept for as long as such a daemon
    /// may still be running; a later version appends its fields at the end of the blob, where this one
    /// skips them.
    void encodeBody(Writer& out, ClientHello const& pdu)
    {
        out.u32(pdu.codecVersion);
        auto fields = Writer {};
        encodeVersionRange(fields, pdu.versions);
        encodeCapabilities(fields, pdu.capabilities);
        fields.string(pdu.token);
        encodeOptionalSessionSettings(fields, pdu.sessionSettings);
        fields.u8(std::to_underlying(pdu.role));
        fields.u8(pdu.clientArea ? 1 : 0);
        if (pdu.clientArea)
        {
            fields.varint(pdu.clientArea->columns);
            fields.varint(pdu.clientArea->lines);
        }
        fields.u8(std::to_underlying(pdu.attachIntent.kind));
        fields.varint(pdu.attachIntent.windows.size());
        for (auto const window: pdu.attachIntent.windows)
            fields.varint(window);
        fields.string(pdu.profileName);
        out.blob(fields.view());
        out.u8(0);
    }

    void encodeBody(Writer& out, SessionSettingsChanged const& pdu)
    {
        encodeOptionalSessionSettings(out, pdu.sessionSettings);
    }
    /// Laid out as HelloLayout says. The version alone is all of a ServerHello a pre-negotiation client
    /// reads, contour 0.7.0's refusing a byte more; a negotiating one puts its fields in one blob, at whose
    /// end a later version appends its own.
    void encodeBody(Writer& out, ServerHello const& pdu)
    {
        out.u32(pdu.codecVersion);
        switch (pdu.layout)
        {
            case HelloLayout::VersionAlone: return;
            case HelloLayout::Negotiating: break;
        }
        auto fields = Writer {};
        fields.varint(pdu.agreedVersion);
        encodeVersionRange(fields, pdu.versions);
        encodeCapabilities(fields, pdu.capabilities);
        fields.string(pdu.machineId);
        fields.string(pdu.hostname);
        fields.string(pdu.releaseVersion);
        fields.varint(pdu.followedWindows.size());
        for (auto const window: pdu.followedWindows)
            fields.varint(window);
        fields.varint(pdu.daemonInstance);
        fields.varint(pdu.restoredWindows);
        fields.varint(pdu.restoredTabs);
        out.blob(fields.view());
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

    /// The optional fields of a command-block record, in WIRE ORDER, with the presence bit each is
    /// gated on — ONE table for both directions, for the reason ContextStringFields gives.
    constexpr auto CommandBlockOptionalFields = std::array {
        std::pair { CommandBlockHasPromptStart, &WireCommandBlock::promptStartedAt },
        std::pair { CommandBlockHasExitCode, &WireCommandBlock::exitCode },
        std::pair { CommandBlockHasCommandStart, &WireCommandBlock::commandStartedAt },
        std::pair { CommandBlockHasDuration, &WireCommandBlock::duration },
    };

    /// Encodes one command-block record, writing only the optionals its `present` mask names.
    void encodeCommandBlock(Writer& out, WireCommandBlock const& block)
    {
        out.varint(block.id);
        out.u8(block.state);
        out.u8(block.end);
        out.u8(block.commandLineSource);
        out.u8(block.locality);
        out.u8(block.present);
        out.string(block.commandLine);
        out.string(block.workingDirectory);
        out.u8(block.endLocality);
        out.string(block.endWorkingDirectory);
        out.u8(block.foreignDirectory);
        out.u8(block.outcomeExit);
        out.u8(block.outcomeSignal);
        out.varint(block.outcomeStatus);
        for (auto const& [bit, member]: CommandBlockOptionalFields)
            if ((block.present & bit) != 0)
                out.svarint(block.*member);
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
        out.svarint(pdu.sessionEpoch);
        out.svarint(pdu.hostTime);
        out.varint(pdu.commandBlockIdEpoch);
        out.varint(pdu.commandBlocksFloor);
        out.varint(pdu.commandBlocks.size());
        for (auto const& block: pdu.commandBlocks)
            encodeCommandBlock(out, block);
        out.string(pdu.interruptedCommand);
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
        out.varint(line.blockId);
        out.varint(line.bornAt);
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
        out.varint(pdu.evictedRowCount);
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
        out.u8(pdu.statusLinesDisplay);
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
        out.varint(pdu.commandBlockIdEpoch);
        out.varint(pdu.stableIdGeneration);
        out.varint(pdu.commandBlocks.size());
        for (auto const& block: pdu.commandBlocks)
            encodeCommandBlock(out, block);
        out.varint(pdu.retiredCommandBlocks.size());
        for (auto const id: pdu.retiredCommandBlocks)
            out.varint(id);
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
        out.u8(std::to_underlying(pdu.occasion));
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
        out.string(pdu.profileName);
    }

    void encodeBody(Writer& out, NewWindow const& pdu)
    {
        out.string(pdu.profileName);
    }

    void encodeBody(Writer& out, SplitPane const& pdu)
    {
        out.varint(pdu.session);
        out.u8(pdu.orientation);
        out.u16(pdu.ratio);
        out.string(pdu.profileName);
    }

    void encodeBody(Writer& out, ClosePane const& pdu)
    {
        out.varint(pdu.session);
    }

    void encodeBody(Writer& out, PerformOperation const& pdu)
    {
        out.varint(pdu.session);
        out.u8(std::to_underlying(pdu.operation));
        out.string(pdu.text);
        out.svarint(pdu.row.stableId);
        out.varint(pdu.row.generation);
    }

    void encodeBody(Writer& out, PermissionRequest const& pdu)
    {
        out.varint(pdu.session);
        out.string(pdu.hostId);
        out.string(pdu.name);
        out.u32(pdu.version);
        out.string(pdu.digest);
        out.varint(pdu.commands.size());
        for (auto const& command: pdu.commands)
        {
            out.string(command.name);
            out.varint(command.argv.size());
            for (auto const& element: command.argv)
                out.string(element);
        }
        out.varint(pdu.control.size());
        for (auto const& permission: pdu.control)
            out.string(permission);
    }

    void encodeBody(Writer& out, PermissionReply const& pdu)
    {
        out.string(pdu.hostId);
        out.string(pdu.digest);
        out.u8(pdu.grantState);
    }

    void encodeBody(Writer& out, Unsupported const& pdu)
    {
        out.varint(pdu.refusedSerial);
        out.varint(pdu.ident);
    }

    void encodeBody(Writer&, DaemonStatusRequest const&)
    {
    }

    void encodeBody(Writer& out, DaemonStatusReply const& pdu)
    {
        out.string(pdu.releaseVersion);
        out.varint(pdu.processId);
        out.varint(pdu.uptimeSeconds);
        out.string(pdu.socketPath);
        out.string(pdu.executablePath);
        out.string(pdu.logFile);
        out.varint(pdu.sessions);
        out.varint(pdu.clients);
        out.u8(pdu.lifecycle);
    }

    void encodeBody(Writer& out, StopDaemon const& pdu)
    {
        out.u8(pdu.reason);
    }

    void encodeBody(Writer& out, EditTab const& pdu)
    {
        out.varint(pdu.session);
        out.u8(std::to_underlying(pdu.kind));
        out.string(pdu.title);
        out.u32(pdu.color);
    }

    void encodeBody(Writer& out, MergeTabIntoSplit const& pdu)
    {
        out.varint(pdu.tabSession);
        out.varint(pdu.targetSession);
        out.u8(pdu.side);
    }

    void encodeBody(Writer& out, Heartbeat const& pdu)
    {
        out.varint(pdu.nonce);
    }

    void encodeBody(Writer& out, ControlRequest const& pdu)
    {
        out.varint(pdu.requestId);
        out.string(pdu.method);
        out.string(pdu.params);
        out.string(pdu.context);
    }

    void encodeBody(Writer& out, ControlReply const& pdu)
    {
        out.varint(pdu.requestId);
        out.u8(std::to_underlying(pdu.outcome));
        out.string(pdu.body);
    }

    void encodeBody(Writer& out, ControlEvent const& pdu)
    {
        out.varint(pdu.subscription);
        out.string(pdu.event);
    }

    void encodeBody(Writer& out, ProgramStatus const& pdu)
    {
        out.varint(pdu.session);
        out.varint(pdu.records.size());
        for (auto const& record: pdu.records)
        {
            out.string(record.id);
            out.u8(record.state);
            out.u8(record.kind);
            out.u8(record.progress);
            out.string(record.app);
            out.string(record.title);
            out.string(record.message);
        }
    }

    void encodeBody(Writer& out, SessionInputEcho const& pdu)
    {
        out.varint(pdu.session);
        out.u8(pdu.echo);
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
            || !assign(in.u8(), settings.sixelScrolling, error)
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

    /// The inverse of encodeOptionalSessionSettings.
    [[nodiscard]] std::expected<std::optional<WireSessionSettings>, DecodeError>
    decodeOptionalSessionSettings(Reader& in)
    {
        auto present = uint8_t {};
        auto error = DecodeError {};
        if (!assign(in.u8(), present, error))
            return std::unexpected(error);
        if (present == 0)
            return std::nullopt;
        return decodeSessionSettings(in).transform(
            [](WireSessionSettings settings) { return std::optional { std::move(settings) }; });
    }

    /// Whose layout the fields after a hello's leading `u32` follow.
    enum class HelloRevision : uint8_t
    {
        PreNegotiation = 0, ///< A pre-negotiation codec's: consumed whole, and left unread.
        Negotiating,        ///< A negotiating peer's: left for the caller to read.
    };

    /// Reads a hello's leading `u32`, the one field every version's hello shares, into @p version. The rest
    /// of a hello of a pre-negotiation codec is consumed whole: what follows its version is that codec's own.
    /// @return Whose layout the rest follows.
    [[nodiscard]] std::expected<HelloRevision, DecodeError> readHelloVersion(Reader& in, uint32_t& version)
    {
        auto error = DecodeError {};
        if (!assign(in.u32(), version, error))
            return std::unexpected(error);
        if (version == NegotiatingHelloMarker)
            return HelloRevision::Negotiating;
        std::ignore = in.bytes(in.remaining()); // cannot fail: it takes exactly what is left
        return HelloRevision::PreNegotiation;
    }

    [[nodiscard]] bool decodeVersionRange(Reader& in, VersionRange& versions, DecodeError& error)
    {
        return assign(in.varint(), versions.min, error) && assign(in.varint(), versions.max, error);
    }

    [[nodiscard]] std::expected<void, DecodeError> decodeCapabilities(Reader& in, CapabilitySet& capabilities)
    {
        return decodeVector(in, capabilities.idents, [](Reader& reader) { return reader.varint(); });
    }

    /// Reads one ClientHello field that follows the capabilities, into @p pdu.
    using ClientHelloField = bool (*)(Reader&, ClientHello&, DecodeError&);

    [[nodiscard]] bool decodeHelloToken(Reader& in, ClientHello& pdu, DecodeError& error)
    {
        return assign(in.string(), pdu.token, error);
    }

    [[nodiscard]] bool decodeHelloSessionSettings(Reader& in, ClientHello& pdu, DecodeError& error)
    {
        auto settings = decodeOptionalSessionSettings(in);
        if (!settings)
        {
            error = settings.error();
            return false;
        }
        pdu.sessionSettings = *std::move(settings);
        return true;
    }

    [[nodiscard]] bool decodeHelloRole(Reader& in, ClientHello& pdu, DecodeError& error)
    {
        // Fails closed: a role this version does not know is never Attach, which spawns a session.
        auto role = uint8_t {};
        if (!assign(in.u8(), role, error))
            return false;
        pdu.role = role == std::to_underlying(ConnectionRole::Attach) ? ConnectionRole::Attach
                                                                      : ConnectionRole::Control;
        return true;
    }

    [[nodiscard]] bool decodeHelloClientArea(Reader& in, ClientHello& pdu, DecodeError& error)
    {
        auto present = uint8_t {};
        if (!assign(in.u8(), present, error))
            return false;
        if (present == 0)
            return true;
        auto area = WireClientArea {};
        if (!assign(in.varint(), area.columns, error) || !assign(in.varint(), area.lines, error))
            return false;
        pdu.clientArea = area;
        return true;
    }

    [[nodiscard]] bool decodeHelloAttachIntent(Reader& in, ClientHello& pdu, DecodeError& error)
    {
        auto kind = uint8_t {};
        if (!assign(in.u8(), kind, error))
            return false;
        auto windows =
            decodeVector(in, pdu.attachIntent.windows, [](Reader& reader) { return reader.varint(); });
        if (!windows)
        {
            error = windows.error();
            return false;
        }
        // A kind no enumerator names is what a daemon without the capability does anyway.
        pdu.attachIntent.kind = kind <= std::to_underlying(AttachIntentKind::Resume)
                                    ? static_cast<AttachIntentKind>(kind)
                                    : AttachIntentKind::AllWindows;
        return true;
    }

    [[nodiscard]] bool decodeHelloProfileName(Reader& in, ClientHello& pdu, DecodeError& error)
    {
        return assign(in.string(), pdu.profileName, error);
    }

    /// The ClientHello fields after the capabilities, in wire order; a field is appended here, never
    /// inserted.
    constexpr auto ClientHelloFields = std::array<ClientHelloField, 6> {
        decodeHelloToken,      decodeHelloSessionSettings, decodeHelloRole,
        decodeHelloClientArea, decodeHelloAttachIntent,    decodeHelloProfileName,
    };

    /// The inverse of encodeBody(ClientHello), which says why the fields travel in a blob. Bytes after the
    /// fields this version knows, inside the blob, are a later version's, and skipped.
    DecodeResult decodeClientHello(Reader& in)
    {
        auto pdu = ClientHello {};
        auto const revision = readHelloVersion(in, pdu.codecVersion);
        if (!revision)
            return std::unexpected(revision.error());
        if (*revision == HelloRevision::PreNegotiation)
        {
            pdu.versions = VersionRange { .min = pdu.codecVersion, .max = pdu.codecVersion };
            return pdu;
        }
        auto const fields = in.blob();
        if (!fields)
            return std::unexpected(fields.error());
        auto inner = Reader { *fields };
        auto error = DecodeError {};
        if (!decodeVersionRange(inner, pdu.versions, error))
            return std::unexpected(error);
        if (auto const capabilities = decodeCapabilities(inner, pdu.capabilities); !capabilities)
            return std::unexpected(capabilities.error());
        // A field the sender predates is missing at the end, and keeps its default: the fields after the
        // capabilities are read in order for as long as the blob has bytes left.
        for (auto const& field: ClientHelloFields)
        {
            if (inner.remaining() == 0)
                break;
            if (!field(inner, pdu, error))
                return std::unexpected(error);
        }
        auto closing = uint8_t {};
        if (!assign(in.u8(), closing, error))
            return std::unexpected(error);
        if (closing != 0)
            return std::unexpected(DecodeError::MalformedPdu);
        return pdu;
    }

    DecodeResult decodeSessionSettingsChanged(Reader& in)
    {
        return decodeOptionalSessionSettings(in).transform([](std::optional<WireSessionSettings> settings) {
            return DecodedPdu { SessionSettingsChanged { .sessionSettings = std::move(settings) } };
        });
    }

    /// The inverse of encodeBody(ServerHello). Bytes after the fields this version knows, inside the blob,
    /// are a later version's, and skipped.
    DecodeResult decodeServerHello(Reader& in)
    {
        auto pdu = ServerHello {};
        auto const revision = readHelloVersion(in, pdu.codecVersion);
        if (!revision)
            return std::unexpected(revision.error());
        if (*revision == HelloRevision::PreNegotiation || in.remaining() == 0)
        {
            pdu.layout = HelloLayout::VersionAlone;
            pdu.versions = VersionRange { .min = pdu.codecVersion, .max = pdu.codecVersion };
            return pdu;
        }
        auto const fields = in.blob();
        if (!fields)
            return std::unexpected(fields.error());
        auto inner = Reader { *fields };
        auto error = DecodeError {};
        if (!assign(inner.varint(), pdu.agreedVersion, error)
            || !decodeVersionRange(inner, pdu.versions, error))
            return std::unexpected(error);
        if (auto const capabilities = decodeCapabilities(inner, pdu.capabilities); !capabilities)
            return std::unexpected(capabilities.error());
        // A field the sender predates is missing at the end, and keeps its default.
        if (inner.remaining() != 0 && !assign(inner.string(), pdu.machineId, error))
            return std::unexpected(error);
        if (inner.remaining() != 0 && !assign(inner.string(), pdu.hostname, error))
            return std::unexpected(error);
        if (inner.remaining() != 0 && !assign(inner.string(), pdu.releaseVersion, error))
            return std::unexpected(error);
        if (inner.remaining() != 0)
        {
            if (auto const followed =
                    decodeVector(inner, pdu.followedWindows, [](Reader& reader) { return reader.varint(); });
                !followed)
                return std::unexpected(followed.error());
        }
        if (inner.remaining() != 0 && !assign(inner.varint(), pdu.daemonInstance, error))
            return std::unexpected(error);
        if (inner.remaining() != 0 && !assign(inner.varint(), pdu.restoredWindows, error))
            return std::unexpected(error);
        if (inner.remaining() != 0 && !assign(inner.varint(), pdu.restoredTabs, error))
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

        // A bit this build does not assign is refused rather than read past, for the reason
        // ContextPresentMask gives.
        if ((context.present | ContextPresentMask) != ContextPresentMask)
            return std::unexpected(DecodeError::MalformedPdu);

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

    /// Decodes one command-block record, reading only the optionals its `present` mask names, and refusing
    /// a mask that names a bit this build does not assign, for the reason ContextPresentMask gives.
    /// Enum-ish bytes are carried through raw, for the consumer to judge.
    /// @see vthost::fromWireCommandBlock
    [[nodiscard]] std::expected<WireCommandBlock, DecodeError> decodeCommandBlock(Reader& in)
    {
        auto block = WireCommandBlock {};
        auto error = DecodeError {};
        if (!assign(in.varint(), block.id, error) || !assign(in.u8(), block.state, error)
            || !assign(in.u8(), block.end, error) || !assign(in.u8(), block.commandLineSource, error)
            || !assign(in.u8(), block.locality, error) || !assign(in.u8(), block.present, error)
            || !assign(in.string(), block.commandLine, error)
            || !assign(in.string(), block.workingDirectory, error)
            || !assign(in.u8(), block.endLocality, error)
            || !assign(in.string(), block.endWorkingDirectory, error)
            || !assign(in.u8(), block.foreignDirectory, error) || !assign(in.u8(), block.outcomeExit, error)
            || !assign(in.u8(), block.outcomeSignal, error)
            || !assign(in.varint(), block.outcomeStatus, error))
            return std::unexpected(error);
        if ((block.present | CommandBlockPresentMask) != CommandBlockPresentMask)
            return std::unexpected(DecodeError::MalformedPdu);
        for (auto const& [bit, member]: CommandBlockOptionalFields)
            if ((block.present & bit) != 0 && !assign(in.svarint(), block.*member, error))
                return std::unexpected(error);
        return block;
    }

    /// One host command-block id, range-checked into its 32 bits.
    [[nodiscard]] std::expected<uint32_t, DecodeError> decodeBlockId(Reader& in)
    {
        auto id = uint32_t {};
        auto error = DecodeError {};
        if (!assign(in.varint(), id, error))
            return std::unexpected(error);
        return id;
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
        if (!assign(in.svarint(), pdu.sessionEpoch, error) || !assign(in.svarint(), pdu.hostTime, error)
            || !assign(in.varint(), pdu.commandBlockIdEpoch, error)
            || !assign(in.varint(), pdu.commandBlocksFloor, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.commandBlocks, decodeCommandBlock); !decoded)
            return std::unexpected(decoded.error());
        // Appended by a later version; an older sender's body ends before it.
        if (in.remaining() != 0 && !assign(in.string(), pdu.interruptedCommand, error))
            return std::unexpected(error);
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
            || !assign(in.u16(), line.contextId, error) || !assign(in.varint(), line.blockId, error)
            || !assign(in.varint(), line.bornAt, error) || !assign(in.svarint(), line.promptEndOffset, error)
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
            || !assign(in.svarint(), pdu.stableFloor, error)
            || !assign(in.varint(), pdu.evictedRowCount, error)
            || !assign(in.svarint(), pdu.cursorLine, error) || !assign(in.svarint(), pdu.cursorColumn, error))
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
            || !assign(in.u8(), pdu.statusLinesChanged, error)
            || !assign(in.u8(), pdu.statusLinesDisplay, error))
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
        if (!assign(in.varint(), pdu.commandBlockIdEpoch, error)
            || !assign(in.varint(), pdu.stableIdGeneration, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.commandBlocks, decodeCommandBlock); !decoded)
            return std::unexpected(decoded.error());
        if (auto const decoded = decodeVector(in, pdu.retiredCommandBlocks, decodeBlockId); !decoded)
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
        auto occasion = uint8_t { 0 };
        if (!assign(in.varint(), pdu.session, error) || !assign(in.string(), pdu.title, error)
            || !assign(in.string(), pdu.body, error) || !assign(in.u8(), occasion, error))
            return std::unexpected(error);
        // Carried, not judged: an occasion this build does not know is the client's to read as unstated,
        // rather than a malformed frame that would end the connection over one notification.
        pdu.occasion = static_cast<NotifyOccasion>(occasion);
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
        // Appended by a later version; an older sender's body ends before it.
        if (in.remaining() != 0 && !assign(in.string(), pdu.profileName, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeNewWindow(Reader& in)
    {
        auto pdu = NewWindow {};
        auto error = DecodeError {};
        if (in.remaining() != 0 && !assign(in.string(), pdu.profileName, error))
            return std::unexpected(error);
        return pdu;
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
        // Appended by a later version; an older sender's body ends before it.
        if (in.remaining() != 0 && !assign(in.string(), pdu.profileName, error))
            return std::unexpected(error);
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

    DecodeResult decodePerformOperation(Reader& in)
    {
        auto pdu = PerformOperation {};
        auto error = DecodeError {};
        auto operation = uint8_t { 0 };
        if (!assign(in.varint(), pdu.session, error) || !assign(in.u8(), operation, error)
            || !assign(in.string(), pdu.text, error) || !assign(in.svarint(), pdu.row.stableId, error)
            || !assign(in.varint(), pdu.row.generation, error))
            return std::unexpected(error);
        // Carried, not judged: an operation this build does not know is the host's to refuse, as an
        // unknown PDU is, rather than a malformed frame that would end the connection.
        pdu.operation = static_cast<EngineOperation>(operation);
        return pdu;
    }

    [[nodiscard]] std::expected<WireDeclaredCommand, DecodeError> decodeDeclaredCommand(Reader& in)
    {
        auto command = WireDeclaredCommand {};
        auto error = DecodeError {};
        if (!assign(in.string(), command.name, error))
            return std::unexpected(error);
        if (auto const decoded =
                decodeVector(in, command.argv, [](Reader& reader) { return reader.string(); });
            !decoded)
            return std::unexpected(decoded.error());
        return command;
    }

    DecodeResult decodePermissionRequest(Reader& in)
    {
        auto pdu = PermissionRequest {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.string(), pdu.hostId, error)
            || !assign(in.string(), pdu.name, error) || !assign(in.u32(), pdu.version, error)
            || !assign(in.string(), pdu.digest, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.commands, decodeDeclaredCommand); !decoded)
            return std::unexpected(decoded.error());
        // Appended by a later version; an older sender's body ends before it.
        if (in.remaining() != 0)
            if (auto const decoded =
                    decodeVector(in, pdu.control, [](Reader& reader) { return reader.string(); });
                !decoded)
                return std::unexpected(decoded.error());
        return pdu;
    }

    DecodeResult decodePermissionReply(Reader& in)
    {
        auto pdu = PermissionReply {};
        auto error = DecodeError {};
        // The answer byte is carried, not judged: vthost::grantStateOf() reads one no enumerator has as no
        // answer, rather than this ending the connection over it.
        if (!assign(in.string(), pdu.hostId, error) || !assign(in.string(), pdu.digest, error)
            || !assign(in.u8(), pdu.grantState, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeUnsupported(Reader& in)
    {
        auto pdu = Unsupported {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.refusedSerial, error) || !assign(in.varint(), pdu.ident, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeDaemonStatusRequest(Reader&)
    {
        return DaemonStatusRequest {};
    }

    DecodeResult decodeDaemonStatusReply(Reader& in)
    {
        auto pdu = DaemonStatusReply {};
        auto error = DecodeError {};
        if (!assign(in.string(), pdu.releaseVersion, error) || !assign(in.varint(), pdu.processId, error)
            || !assign(in.varint(), pdu.uptimeSeconds, error) || !assign(in.string(), pdu.socketPath, error)
            || !assign(in.string(), pdu.executablePath, error) || !assign(in.string(), pdu.logFile, error)
            || !assign(in.varint(), pdu.sessions, error) || !assign(in.varint(), pdu.clients, error)
            || !assign(in.u8(), pdu.lifecycle, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeStopDaemon(Reader& in)
    {
        auto pdu = StopDaemon {};
        auto error = DecodeError {};
        if (!assign(in.u8(), pdu.reason, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeEditTab(Reader& in)
    {
        auto pdu = EditTab {};
        auto error = DecodeError {};
        auto kind = uint8_t {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.u8(), kind, error)
            || !assign(in.string(), pdu.title, error) || !assign(in.u32(), pdu.color, error))
            return std::unexpected(error);
        // Carried, not judged: the host refuses a kind it does not know.
        pdu.kind = static_cast<TabEditKind>(kind);
        return pdu;
    }

    DecodeResult decodeMergeTabIntoSplit(Reader& in)
    {
        auto pdu = MergeTabIntoSplit {};
        auto error = DecodeError {};
        // The side is carried, not judged: the host refuses a byte it does not know.
        if (!assign(in.varint(), pdu.tabSession, error) || !assign(in.varint(), pdu.targetSession, error)
            || !assign(in.u8(), pdu.side, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeHeartbeat(Reader& in)
    {
        auto pdu = Heartbeat {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.nonce, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeControlRequest(Reader& in)
    {
        auto pdu = ControlRequest {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.requestId, error) || !assign(in.string(), pdu.method, error)
            || !assign(in.string(), pdu.params, error) || !assign(in.string(), pdu.context, error))
            return std::unexpected(error);
        return pdu;
    }

    DecodeResult decodeControlReply(Reader& in)
    {
        auto pdu = ControlReply {};
        auto error = DecodeError {};
        auto outcome = uint8_t {};
        if (!assign(in.varint(), pdu.requestId, error) || !assign(in.u8(), outcome, error)
            || !assign(in.string(), pdu.body, error))
            return std::unexpected(error);
        // An outcome this build does not know is an error: the body of a future outcome is not a reply.
        pdu.outcome =
            outcome == std::to_underlying(ControlOutcome::Ok) ? ControlOutcome::Ok : ControlOutcome::Error;
        return pdu;
    }

    DecodeResult decodeControlEvent(Reader& in)
    {
        auto pdu = ControlEvent {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.subscription, error) || !assign(in.string(), pdu.event, error))
            return std::unexpected(error);
        return pdu;
    }

    [[nodiscard]] std::expected<WireProgramStatusRecord, DecodeError> decodeProgramStatusRecord(Reader& in)
    {
        auto record = WireProgramStatusRecord {};
        auto error = DecodeError {};
        if (!assign(in.string(), record.id, error) || !assign(in.u8(), record.state, error)
            || !assign(in.u8(), record.kind, error) || !assign(in.u8(), record.progress, error)
            || !assign(in.string(), record.app, error) || !assign(in.string(), record.title, error)
            || !assign(in.string(), record.message, error))
            return std::unexpected(error);
        return record;
    }

    DecodeResult decodeProgramStatus(Reader& in)
    {
        auto pdu = ProgramStatus {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error))
            return std::unexpected(error);
        if (auto const decoded = decodeVector(in, pdu.records, decodeProgramStatusRecord); !decoded)
            return std::unexpected(decoded.error());
        return pdu;
    }

    DecodeResult decodeSessionInputEcho(Reader& in)
    {
        // The value is carried, not judged: one this build does not know reads as Unknown where it is used.
        auto pdu = SessionInputEcho {};
        auto error = DecodeError {};
        if (!assign(in.varint(), pdu.session, error) || !assign(in.u8(), pdu.echo, error))
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
        DecodeRow { PduType::SessionSettingsChanged, decodeSessionSettingsChanged },
        DecodeRow { PduType::PerformOperation, decodePerformOperation },
        DecodeRow { PduType::PermissionRequest, decodePermissionRequest },
        DecodeRow { PduType::PermissionReply, decodePermissionReply },
        DecodeRow { PduType::Unsupported, decodeUnsupported },
        DecodeRow { PduType::DaemonStatusRequest, decodeDaemonStatusRequest },
        DecodeRow { PduType::DaemonStatusReply, decodeDaemonStatusReply },
        DecodeRow { PduType::StopDaemon, decodeStopDaemon },
        DecodeRow { PduType::EditTab, decodeEditTab },
        DecodeRow { PduType::Heartbeat, decodeHeartbeat },
        DecodeRow { PduType::ControlRequest, decodeControlRequest },
        DecodeRow { PduType::ControlReply, decodeControlReply },
        DecodeRow { PduType::ControlEvent, decodeControlEvent },
        DecodeRow { PduType::ProgramStatus, decodeProgramStatus },
        DecodeRow { PduType::SessionInputEcho, decodeSessionInputEcho },
        DecodeRow { PduType::MergeTabIntoSplit, decodeMergeTabIntoSplit },
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
    // header is three svarints, two u16, four varints (the block id and birth stamp among them) and
    // four u32. Both are rounded up to a round number rather than computed exactly; the declaration
    // says why an estimate is the point.
    constexpr auto BytesPerCell = std::size_t { 24 };
    constexpr auto BytesPerRowHeader = std::size_t { 32 };
    return BytesPerRowHeader + (line.cells.size() * BytesPerCell);
}

std::size_t estimatedEncodedSize(WireCommandBlock const& block) noexcept
{
    // encodeCommandBlock writes at most: a 5-byte varint id, six u8, three string lengths (5-byte
    // varints for any string shorter than 32 GiB), three u8, a 10-byte varint status and four 10-byte
    // svarint optionals = 79, plus the three strings' bytes. Every term is its widest encoding: this one
    // must never under-count.
    constexpr auto FixedBytes = std::size_t { 79 };
    return FixedBytes + block.commandLine.size() + block.workingDirectory.size()
           + block.endWorkingDirectory.size();
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

    // Whatever is left of the body is a newer peer's: bodies grow by appending fields (see Pdu.hpp).

    return DecodedFrame {
        .serial = frame->serial,
        .pdu = std::move(*pdu),
        .consumed = frame->consumed,
    };
}

} // namespace vthost::proto
