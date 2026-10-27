// SPDX-License-Identifier: Apache-2.0
#include <vthost/proto/PduTrace.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <utility>
#include <variant>

namespace vthost::proto
{

namespace
{
    /// @return @p range as `min-max`.
    [[nodiscard]] std::string formatRange(VersionRange range)
    {
        return std::format("{}-{}", range.min, range.max);
    }

    /// @return The idents of @p capabilities, comma-separated, or `none`.
    [[nodiscard]] std::string formatCapabilities(CapabilitySet const& capabilities)
    {
        if (capabilities.idents.empty())
            return "none";
        auto text = std::string {};
        for (auto const ident: capabilities.idents)
            text += std::format("{}{}", text.empty() ? "" : ",", ident);
        return text;
    }

    /// @return @p windows as `[1,3]`.
    [[nodiscard]] std::string formatWindows(std::vector<uint64_t> const& windows)
    {
        auto text = std::string {};
        for (auto const window: windows)
            text += std::format("{}{}", text.empty() ? "" : ",", window);
        return std::format("[{}]", text);
    }

    /// One row of the table naming an AttachIntentKind in a trace.
    struct IntentName
    {
        AttachIntentKind kind;
        std::string_view name;
    };

    constexpr auto IntentNames = std::array {
        IntentName { .kind = AttachIntentKind::AllWindows, .name = "all-windows" },
        IntentName { .kind = AttachIntentKind::Launch, .name = "launch" },
        IntentName { .kind = AttachIntentKind::NewWindow, .name = "new-window" },
        IntentName { .kind = AttachIntentKind::Windows, .name = "windows" },
        IntentName { .kind = AttachIntentKind::Resume, .name = "resume" },
    };

    /// One row of the table naming a TabEditKind in a trace.
    struct TabEditName
    {
        TabEditKind kind;
        std::string_view name;
    };

    constexpr auto TabEditNames = std::array {
        TabEditName { .kind = TabEditKind::ActivatePane, .name = "activate-pane" },
        TabEditName { .kind = TabEditKind::Zoom, .name = "zoom" },
        TabEditName { .kind = TabEditKind::Unzoom, .name = "unzoom" },
        TabEditName { .kind = TabEditKind::SetTitle, .name = "set-title" },
        TabEditName { .kind = TabEditKind::ClearTitle, .name = "clear-title" },
        TabEditName { .kind = TabEditKind::SetColor, .name = "set-color" },
        TabEditName { .kind = TabEditKind::ClearColor, .name = "clear-color" },
        TabEditName { .kind = TabEditKind::ActivateTab, .name = "activate-tab" },
    };

    /// @return The profile as a trace field (`profile="x"`), or nothing for the connection's own.
    [[nodiscard]] std::string profileField(std::string const& profileName)
    {
        return profileName.empty() ? std::string {} : std::format("profile=\"{}\"", profileName);
    }

    /// @return The profile field with the space that separates it from the fields before it, or nothing.
    [[nodiscard]] std::string profileSuffix(std::string const& profileName)
    {
        return profileName.empty() ? std::string {} : std::format(" {}", profileField(profileName));
    }

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
                       return std::format(
                           "versions={} caps={} token={} settings={} role={} area={} intent={} windows={} "
                           "profile=\"{}\"",
                           formatRange(value.versions),
                           formatCapabilities(value.capabilities),
                           value.token.empty() ? "no" : "yes",
                           value.sessionSettings ? "yes" : "no",
                           value.role == ConnectionRole::Control ? "control" : "attach",
                           value.clientArea
                               ? std::format("{}x{}", value.clientArea->columns, value.clientArea->lines)
                               : std::string { "none" },
                           std::ranges::find(IntentNames, value.attachIntent.kind, &IntentName::kind)->name,
                           formatWindows(value.attachIntent.windows),
                           value.profileName);
                   } },
        TraceRow { PduType::ServerHello,
                   "ServerHello",
                   +[](DecodedPdu const& pdu) {
                       // The daemon's machine id and host name name a machine: left out altogether. Its
                       // release version names no one, and is how a drift between the ends is diagnosed.
                       auto const& value = std::get<ServerHello>(pdu);
                       switch (value.layout)
                       {
                           case HelloLayout::VersionAlone:
                               return std::format("layout=version-alone version={}", value.codecVersion);
                           case HelloLayout::Negotiating: break;
                       }
                       return std::format("layout=negotiating agreed={} versions={} caps={} release={} "
                                          "followed={} instance={:016x} restored={}/{}",
                                          value.agreedVersion,
                                          formatRange(value.versions),
                                          formatCapabilities(value.capabilities),
                                          value.releaseVersion.empty() ? "unknown" : value.releaseVersion,
                                          formatWindows(value.followedWindows),
                                          value.daemonInstance,
                                          value.restoredWindows,
                                          value.restoredTabs);
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
                       // Context records carry user, hostname, cwd and cmdline: a COUNT is the
                       // diagnostic, and the fields themselves must never reach a trace log.
                       // Command-block records carry command lines and directories: counted, never
                       // shown; so is the interrupted command's length.
                       return std::format(
                           "session={} {}x{} screen={} title={}ch cwd={}ch progress={}/{} contexts={} "
                           "chain={} blocks={} interrupted={}ch",
                           value.session,
                           value.columns,
                           value.lines,
                           value.screenType,
                           value.title.size(),
                           value.cwd.size(),
                           value.progressState,
                           value.progressPercentage,
                           value.contexts.size(),
                           value.contextChain.size(),
                           value.commandBlocks.size(),
                           value.interruptedCommand.size());
                   } },
        TraceRow { PduType::Delta,
                   "Delta",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<Delta>(pdu);
                       // Counts and the ACTIVE ID only -- never a record's fields. @see the
                       // SessionState row above.
                       return std::format("session={} gen={} seq={} snapshot={}/{} lines={} links={} "
                                          "imagecells={} statuslines={} progress={}/{} contexts={} "
                                          "activecontext={} blocks={} retired={}",
                                          value.session,
                                          value.generation,
                                          value.seqno,
                                          value.snapshot,
                                          value.snapshotPart,
                                          value.lines.size(),
                                          value.hyperlinks.size(),
                                          value.imageCells.size(),
                                          value.statusLines.size(),
                                          value.progressChanged != 0 ? value.progressState : 0,
                                          value.progressChanged != 0 ? value.progressPercentage : 0,
                                          value.contexts.size(),
                                          value.contextChanged != 0 ? value.activeContext : 0,
                                          value.commandBlocks.size(),
                                          value.retiredCommandBlocks.size());
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
                       return std::format("session={} title={}ch body={}ch occasion={}",
                                          value.session,
                                          value.title.size(),
                                          value.body.size(),
                                          std::to_underlying(value.occasion));
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
                       auto const& value = std::get<CreateTab>(pdu);
                       return std::format("session={}{}", value.session, profileSuffix(value.profileName));
                   } },
        TraceRow { PduType::SplitPane,
                   "SplitPane",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<SplitPane>(pdu);
                       return std::format("session={} orientation={} ratio={}{}",
                                          value.session,
                                          value.orientation,
                                          value.ratio,
                                          profileSuffix(value.profileName));
                   } },
        TraceRow { PduType::ClosePane,
                   "ClosePane",
                   +[](DecodedPdu const& pdu) {
                       return std::format("session={}", std::get<ClosePane>(pdu).session);
                   } },
        TraceRow { PduType::NewWindow,
                   "NewWindow",
                   +[](DecodedPdu const& pdu) {
                       // Empty for the connection's own profile, which keeps a plain NewWindow's trace bare.
                       return profileField(std::get<NewWindow>(pdu).profileName);
                   } },
        TraceRow { PduType::ResizeSplit,
                   "ResizeSplit",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ResizeSplit>(pdu);
                       return std::format("first={} second={} ratio={}",
                                          value.firstSession,
                                          value.secondSession,
                                          value.ratio);
                   } },
        TraceRow { PduType::SessionSettingsChanged,
                   "SessionSettingsChanged",
                   +[](DecodedPdu const& pdu) {
                       // Summarized, never transcribed, as ClientHello's are.
                       return std::format("settings={}",
                                          std::get<SessionSettingsChanged>(pdu).sessionSettings ? "yes"
                                                                                                : "no");
                   } },
        TraceRow { PduType::PerformOperation,
                   "PerformOperation",
                   +[](DecodedPdu const& pdu) {
                       // The text is sized, never transcribed: it is what lands on the user's screen.
                       auto const& value = std::get<PerformOperation>(pdu);
                       return std::format("session={} operation={} text={}B row={}@{}",
                                          value.session,
                                          std::to_underlying(value.operation),
                                          value.text.size(),
                                          value.row.stableId,
                                          value.row.generation);
                   } },
        TraceRow { PduType::PermissionRequest,
                   "PermissionRequest",
                   +[](DecodedPdu const& pdu) {
                       // The manifest is counted, never transcribed: its text is the guest's own.
                       auto const& value = std::get<PermissionRequest>(pdu);
                       return std::format("session={} digest={} commands={} control={}",
                                          value.session,
                                          value.digest,
                                          value.commands.size(),
                                          value.control.size());
                   } },
        TraceRow { PduType::PermissionReply,
                   "PermissionReply",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<PermissionReply>(pdu);
                       return std::format("digest={} grant={}", value.digest, value.grantState);
                   } },
        TraceRow { PduType::Unsupported,
                   "Unsupported",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<Unsupported>(pdu);
                       return std::format("refused={} ident={}", value.refusedSerial, value.ident);
                   } },
        TraceRow { PduType::DaemonStatusRequest, "DaemonStatusRequest", nullptr },
        TraceRow { PduType::DaemonStatusReply,
                   "DaemonStatusReply",
                   +[](DecodedPdu const& pdu) {
                       // The socket, executable and log paths name the user's home: left out.
                       auto const& value = std::get<DaemonStatusReply>(pdu);
                       return std::format("release={} pid={} uptime={}s sessions={} clients={} lifecycle={}",
                                          value.releaseVersion,
                                          value.processId,
                                          value.uptimeSeconds,
                                          value.sessions,
                                          value.clients,
                                          value.lifecycle);
                   } },
        TraceRow { PduType::StopDaemon,
                   "StopDaemon",
                   +[](DecodedPdu const& pdu) {
                       return std::format("reason={}", std::get<StopDaemon>(pdu).reason);
                   } },
        TraceRow { PduType::EditTab,
                   "EditTab",
                   +[](DecodedPdu const& pdu) {
                       // The title is sized, never transcribed: it is the user's own text.
                       auto const& value = std::get<EditTab>(pdu);
                       auto const known = std::ranges::find(TabEditNames, value.kind, &TabEditName::kind);
                       auto text = std::format("session={} {}",
                                               value.session,
                                               known != TabEditNames.end()
                                                   ? std::string { known->name }
                                                   : std::format("kind={}", std::to_underlying(value.kind)));
                       if (value.kind == TabEditKind::SetTitle)
                           text += std::format(" title={}B", value.title.size());
                       if (value.kind == TabEditKind::SetColor)
                           text += std::format(" #{:06x}", value.color);
                       return text;
                   } },
        TraceRow {
            PduType::Heartbeat,
            "Heartbeat",
            +[](DecodedPdu const& pdu) { return std::format("nonce={}", std::get<Heartbeat>(pdu).nonce); } },
        // The call's text is sized, never transcribed: params, bodies and events can hold pane text.
        TraceRow { PduType::ControlRequest,
                   "ControlRequest",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ControlRequest>(pdu);
                       return std::format("id={} method={} params={}B context={}B",
                                          value.requestId,
                                          value.method,
                                          value.params.size(),
                                          value.context.size());
                   } },
        TraceRow { PduType::ControlReply,
                   "ControlReply",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ControlReply>(pdu);
                       return std::format("id={} {} body={}B",
                                          value.requestId,
                                          value.outcome == ControlOutcome::Ok ? "ok" : "error",
                                          value.body.size());
                   } },
        TraceRow { PduType::ControlEvent,
                   "ControlEvent",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ControlEvent>(pdu);
                       return std::format(
                           "subscription={} event={}B", value.subscription, value.event.size());
                   } },
        // A program's text is counted, never transcribed.
        TraceRow { PduType::ProgramStatus,
                   "ProgramStatus",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<ProgramStatus>(pdu);
                       return std::format("session={} records={}", value.session, value.records.size());
                   } },
        TraceRow { PduType::SessionInputEcho,
                   "SessionInputEcho",
                   +[](DecodedPdu const& pdu) {
                       auto const& value = std::get<SessionInputEcho>(pdu);
                       return std::format("session={} echo={}", value.session, value.echo);
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

std::string nameOfIdent(std::uint64_t ident)
{
    // Invalid stands for an off-catalog ident; it is no tag of its own.
    auto const row = std::ranges::find_if(TraceTable, [ident](TraceRow const& candidate) {
        return candidate.tag != PduType::Invalid && std::to_underlying(candidate.tag) == ident;
    });
    return row != TraceTable.end() ? std::string { row->name } : std::to_string(ident);
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
