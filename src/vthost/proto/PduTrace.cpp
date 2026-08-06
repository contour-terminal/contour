// SPDX-License-Identifier: Apache-2.0
#include <vthost/proto/PduTrace.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <variant>

namespace vthost::proto
{

namespace
{
    /// The trace half of the catalog: one row per tag, carrying BOTH the name and how to
    /// summarise the payload — so adding a PDU is one row here, not a row in one table and an
    /// overload somewhere else. The captureless lambdas decay to plain function pointers via
    /// the unary +, which is what lets behaviour sit in the table beside the data.
    struct TraceRow
    {
        PduType tag;
        std::string_view name;
        /// How to render the payload's identifying fields; NULL for a payloadless PDU.
        std::string (*summarize)(DecodedPdu const&);
    };

    constexpr auto TraceTable = std::array {
        TraceRow {
            PduType::Invalid,
            "Invalid",
            +[](DecodedPdu const& pdu) { return std::format("ident={}", std::get<Invalid>(pdu).ident); } },
        TraceRow { PduType::ClientHello,
                   "ClientHello",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ClientHello>(pdu);
                       // The token authenticates the peer: report its PRESENCE, never its bytes. The
                       // session settings get the same treatment for a different reason -- they are
                       // a block of the user's configuration, and a trace is not a config dump.
                       return std::format("version={} token={} settings={}",
                                          value.codecVersion,
                                          value.token.empty() ? "no" : "yes",
                                          value.sessionSettings ? "yes" : "no");
                   } },
        TraceRow { PduType::ServerHello,
                   "ServerHello",
                   +[](DecodedPdu const& pdu) {
                       return std::format("version={}", std::get<ServerHello>(pdu).codecVersion);
                   } },
        TraceRow { PduType::Input,
                   "Input",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<Input>(pdu);
                       // Byte COUNT only — these are the user's keystrokes.
                       return std::format("session={} bytes={}", value.session, value.data.size());
                   } },
        TraceRow { PduType::ResizeRequest,
                   "ResizeRequest",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ResizeRequest>(pdu);
                       return std::format("cols={} lines={}", value.columns, value.lines);
                   } },
        TraceRow { PduType::ResizePane,
                   "ResizePane",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ResizePane>(pdu);
                       return std::format(
                           "session={} cols={} lines={}", value.session, value.columns, value.lines);
                   } },
        TraceRow { PduType::FetchImage,
                   "FetchImage",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<FetchImage>(pdu);
                       return std::format("session={} image={}", value.session, value.imageId);
                   } },
        TraceRow { PduType::ImageData,
                   "ImageData",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ImageData>(pdu);
                       return std::format("image={} format={} {}x{}px bytes={}",
                                          value.imageId,
                                          value.format,
                                          value.width,
                                          value.height,
                                          value.data.size());
                   } },
        TraceRow { PduType::ImageGone,
                   "ImageGone",
                   +[](DecodedPdu const& pdu) {
                       return std::format("image={}", std::get<ImageGone>(pdu).imageId);
                   } },
        TraceRow { PduType::SessionState,
                   "SessionState",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<SessionState>(pdu);
                       // title/cwd are screen-derived content; their LENGTHS are the diagnostic.
                       return std::format("session={} {}x{} screen={} title={}ch cwd={}ch",
                                          value.session,
                                          value.columns,
                                          value.lines,
                                          value.screenType,
                                          value.title.size(),
                                          value.cwd.size());
                   } },
        TraceRow { PduType::Delta,
                   "Delta",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<Delta>(pdu);
                       return std::format("session={} gen={} seq={} snapshot={} lines={} links={} "
                                          "imagecells={} statuslines={}",
                                          value.session,
                                          value.generation,
                                          value.seqno,
                                          value.snapshot,
                                          value.lines.size(),
                                          value.hyperlinks.size(),
                                          value.imageCells.size(),
                                          value.statusLines.size());
                   } },
        TraceRow { PduType::SessionBell,
                   "SessionBell",
                   +[](DecodedPdu const& pdu) {
                       return std::format("session={}", std::get<SessionBell>(pdu).session);
                   } },
        TraceRow { PduType::SessionNotify,
                   "SessionNotify",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<SessionNotify>(pdu);
                       // A notification's text is the user's content: sizes only, never the bytes.
                       return std::format("session={} title={}ch body={}ch",
                                          value.session,
                                          value.title.size(),
                                          value.body.size());
                   } },
        TraceRow { PduType::SessionClipboard,
                   "SessionClipboard",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<SessionClipboard>(pdu);
                       // Clipboard data is the user's content, and often a password: size only. The
                       // selection is a single protocol letter and safe to name.
                       return std::format("session={} selection={} data={}ch",
                                          value.session,
                                          value.selection,
                                          value.data.size());
                   } },
        TraceRow { PduType::LayoutState,
                   "LayoutState",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<LayoutState>(pdu);
                       return std::format("window={} tabs={} activetab={}",
                                          value.window,
                                          value.tabs.size(),
                                          value.activeTab);
                   } },
        TraceRow { PduType::CreateTab,
                   "CreateTab",
                   +[](DecodedPdu const& pdu) {
                       return std::format("session={}", std::get<CreateTab>(pdu).session);
                   } },
        TraceRow { PduType::SplitPane,
                   "SplitPane",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<SplitPane>(pdu);
                       return std::format("session={} orientation={} ratio={}",
                                          value.session,
                                          value.orientation,
                                          value.ratio);
                   } },
        TraceRow { PduType::ClosePane,
                   "ClosePane",
                   +[](DecodedPdu const& pdu) {
                       return std::format("session={}", std::get<ClosePane>(pdu).session);
                   } },
        TraceRow { PduType::NewWindow, "NewWindow", nullptr },
        TraceRow { PduType::ResizeSplit,
                   "ResizeSplit",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ResizeSplit>(pdu);
                       return std::format("first={} second={} ratio={}",
                                          value.firstSession,
                                          value.secondSession,
                                          value.ratio);
                   } },
    };

    static_assert(TraceTable.size() == std::variant_size_v<DecodedPdu>,
                  "every DecodedPdu alternative needs a TraceTable row");

    /// @param tag The catalog tag to look up.
    /// @return Its row, or nullptr for an off-catalog value.
    [[nodiscard]] TraceRow const* findRow(PduType tag) noexcept
    {
        auto const row = std::ranges::find(TraceTable, tag, &TraceRow::tag);
        return row != TraceTable.end() ? &*row : nullptr;
    }
} // namespace

std::string_view toString(PduType type) noexcept
{
    auto const* const row = findRow(type);
    return row != nullptr ? row->name : std::string_view { "Unknown" };
}

std::string summarize(DecodedPdu const& pdu)
{
    auto const* const row = findRow(typeOf(pdu));
    return row != nullptr && row->summarize != nullptr ? row->summarize(pdu) : std::string {};
}

std::string describe(DecodedPdu const& pdu)
{
    auto const* const row = findRow(typeOf(pdu));
    auto const name = row != nullptr ? row->name : std::string_view { "Unknown" };
    if (row == nullptr || row->summarize == nullptr)
        return std::string { name };
    auto const fields = row->summarize(pdu);
    return fields.empty() ? std::string { name } : std::format("{} {}", name, fields);
}

std::string traceLine(Direction direction, std::uint64_t serial, DecodedPdu const& pdu, std::size_t wireBytes)
{
    // describe() does the one table lookup for both halves; going through summarize() and
    // toString() separately would visit the variant twice and scan the table twice, on a path
    // that runs once per PDU.
    return std::format("{} #{} {} ({} bytes)",
                       direction == Direction::Recv ? "recv" : "send",
                       serial,
                       describe(pdu),
                       wireBytes);
}

} // namespace vthost::proto
