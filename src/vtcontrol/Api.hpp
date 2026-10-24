// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vtcontrol/EnumTable.hpp>

namespace vtcontrol
{

/// Marks a type as a record of the control API: a request, a reply, or a vocabulary struct. Phase 7c's
/// WIT renderer recognises API records by it.
template <typename T>
concept ApiRecord = requires { typename T::ApiRecordTag; };

/// A JSON document carried verbatim inside another: written as it stands, read as the exact text of one
/// value. What a reply holds when its shape is not a C++ struct -- a schema, an example.
struct RawJson
{
    std::string text {}; ///< The JSON text.

    [[nodiscard]] bool operator==(RawJson const&) const = default;
};

// {{{ Enums
/// Whether a new tab or pane takes the focus.
enum class FocusPolicy : uint8_t
{
    Take = 0,
    Keep,
};

template <>
struct EnumTable<FocusPolicy>
{
    static constexpr std::array Rows {
        EnumName { FocusPolicy::Take, "take" },
        EnumName { FocusPolicy::Keep, "keep" },
    };
};

/// Where a new tab goes.
enum class WindowPlacement : uint8_t
{
    Existing = 0,
    New,
};

template <>
struct EnumTable<WindowPlacement>
{
    static constexpr std::array Rows {
        EnumName { WindowPlacement::Existing, "existing" },
        EnumName { WindowPlacement::New, "new" },
    };
};

/// Where the new pane goes relative to the one split.
enum class SplitDirection : uint8_t
{
    Right = 0,
    Below,
};

template <>
struct EnumTable<SplitDirection>
{
    static constexpr std::array Rows {
        EnumName { SplitDirection::Right, "right" },
        EnumName { SplitDirection::Below, "below" },
    };
};

/// What `pane.send` waits for before it replies.
enum class SendWait : uint8_t
{
    None = 0,
    CommandFinished,
};

template <>
struct EnumTable<SendWait>
{
    static constexpr std::array Rows {
        EnumName { SendWait::None, "none" },
        EnumName { SendWait::CommandFinished, "commandFinished" },
    };
};

/// How far a command block has come.
enum class BlockState : uint8_t
{
    Prompting = 0,
    Running,
    Finished,
    Superseded,
};

template <>
struct EnumTable<BlockState>
{
    static constexpr std::array Rows {
        EnumName { BlockState::Prompting, "prompting" },
        EnumName { BlockState::Running, "running" },
        EnumName { BlockState::Finished, "finished" },
        EnumName { BlockState::Superseded, "superseded" },
    };
};

/// Which part of a block to read.
enum class BlockPart : uint8_t
{
    All = 0,
    Prompt,
    Output,
};

template <>
struct EnumTable<BlockPart>
{
    static constexpr std::array Rows {
        EnumName { BlockPart::All, "all" },
        EnumName { BlockPart::Prompt, "prompt" },
        EnumName { BlockPart::Output, "output" },
    };
};

/// Which end of a reply was cut to fit the reply cap.
enum class Truncation : uint8_t
{
    None = 0,
    Head,
};

template <>
struct EnumTable<Truncation>
{
    static constexpr std::array Rows {
        EnumName { Truncation::None, "none" },
        EnumName { Truncation::Head, "head" },
    };
};

/// What a program running in a pane says it is doing.
enum class ProgramState : uint8_t
{
    Idle = 0,
    Working,
    Done,
    Blocked,
    Error,
};

template <>
struct EnumTable<ProgramState>
{
    static constexpr std::array Rows {
        EnumName { ProgramState::Idle, "idle" },   EnumName { ProgramState::Working, "working" },
        EnumName { ProgramState::Done, "done" },   EnumName { ProgramState::Blocked, "blocked" },
        EnumName { ProgramState::Error, "error" },
    };
};

/// Why a program is blocked.
enum class BlockedKind : uint8_t
{
    None = 0,
    Permission,
    Question,
    Auth,
};

template <>
struct EnumTable<BlockedKind>
{
    static constexpr std::array Rows {
        EnumName { BlockedKind::None, "none" },
        EnumName { BlockedKind::Permission, "permission" },
        EnumName { BlockedKind::Question, "question" },
        EnumName { BlockedKind::Auth, "auth" },
    };
};

/// Why a program-status record changed.
enum class ProgramStatusReason : uint8_t
{
    Report = 0,
    Prompt,
    ProcessExit,
    Reset,
    Mirror,
};

template <>
struct EnumTable<ProgramStatusReason>
{
    static constexpr std::array Rows {
        EnumName { ProgramStatusReason::Report, "report" },
        EnumName { ProgramStatusReason::Prompt, "prompt" },
        EnumName { ProgramStatusReason::ProcessExit, "processExit" },
        EnumName { ProgramStatusReason::Reset, "reset" },
        EnumName { ProgramStatusReason::Mirror, "mirror" },
    };
};

// }}}

// {{{ Vocabulary records

/// A terminal size.
struct Size
{
    using ApiRecordTag = void;

    uint32_t columns = 0; ///< Width in cells.
    uint32_t lines = 0;   ///< Height in cells.
};

/// A cursor position; `line` is negative for history.
struct Position
{
    using ApiRecordTag = void;

    int32_t line = 0;    ///< Line, 0 being the top of the page.
    uint32_t column = 0; ///< Column, 0 being the leftmost.
};

/// One program-status record of a pane (filled in by phase 7b; empty until then).
struct ProgramStatusInfo
{
    using ApiRecordTag = void;

    std::string id {};                       ///< The record's id, scoped to its pane.
    ProgramState state = ProgramState::Idle; ///< What the program is doing.
    BlockedKind kind = BlockedKind::None;    ///< Why it is blocked, when `state` is `blocked`.
    std::optional<uint8_t> progress {};      ///< Percent done, when known.
    std::string app {};                      ///< The program's name.
    std::string title {};                    ///< A short title.
    std::string message {};                  ///< A longer message.
};

/// A pane: one hosted shell.
struct PaneInfo
{
    using ApiRecordTag = void;

    std::string id {};     ///< `pane:<uuid>`.
    std::string label {};  ///< The label, or empty.
    std::string title {};  ///< The window title the shell set.
    std::string cwd {};    ///< The shell's working directory, or empty when unknown.
    bool isActive = false; ///< Whether it is its tab's active pane.
    Size size {};          ///< Its size in cells.
    std::vector<ProgramStatusInfo> programStatus {}; ///< Program-status records; empty until phase 7b.
};

/// A tab.
struct TabInfo
{
    using ApiRecordTag = void;

    std::string id {};              ///< `tab:<uuid>`.
    std::string label {};           ///< The label, or empty.
    std::string title {};           ///< The tab's title.
    uint32_t position = 0;          ///< 1-based position within the window.
    bool isActive = false;          ///< Whether it is its window's active tab.
    std::vector<PaneInfo> panes {}; ///< Its panes.
};

/// A window.
struct WindowInfo
{
    using ApiRecordTag = void;

    std::string id {};            ///< `window:<uuid>`.
    std::string label {};         ///< The label, or empty.
    uint32_t position = 0;        ///< 1-based position among the daemon's windows.
    std::vector<TabInfo> tabs {}; ///< Its tabs.
};

/// A command block.
struct BlockInfo
{
    using ApiRecordTag = void;

    std::string id {};                        ///< `block:<pane-uuid>/<serial>`.
    std::string pane {};                      ///< The pane's id.
    BlockState state = BlockState::Prompting; ///< How far it has come.
    std::optional<std::string> command {};    ///< The command line, when known.
    std::optional<int32_t> exitCode {};       ///< The exit status, once finished.
    std::optional<std::string> cwd {};        ///< The working directory it ran in.
    std::optional<std::string> promptAt {};   ///< RFC 3339 UTC time the prompt started.
    std::optional<std::string> startedAt {};  ///< RFC 3339 UTC time the command started.
    std::optional<uint32_t> durationMs {};    ///< How long it ran, in milliseconds.
};

// }}}
// {{{ Requests and replies

/// A reply that carries nothing.
struct EmptyReply
{
    using ApiRecordTag = void;
};

/// `daemon.info` request.
struct DaemonInfoRequest
{
    using ApiRecordTag = void;
};

/// `daemon.info` reply.
struct DaemonInfoReply
{
    using ApiRecordTag = void;

    std::string version {};       ///< The release version.
    uint32_t protocolVersion = 0; ///< The native protocol version.
    std::string platform {};      ///< The operating system.
    std::string hostname {};      ///< The host's name.
    uint32_t uptimeSeconds = 0;   ///< Seconds since the daemon started.
};

/// `list` request.
struct ListRequest
{
    using ApiRecordTag = void;

    std::string window {}; ///< Restrict to this window; empty for all.
};

/// `list` reply.
struct ListReply
{
    using ApiRecordTag = void;

    std::vector<WindowInfo> windows {}; ///< The windows.
};

/// `tab.create` request.
struct CreateTabRequest
{
    using ApiRecordTag = void;

    std::string window {};                                 ///< The window; empty for the primary viewer's.
    WindowPlacement placement = WindowPlacement::Existing; ///< Existing window or a new one.
    std::string cwd {};                                    ///< Working directory; empty for the default.
    std::vector<std::string> command {};                   ///< Program and arguments; empty for the shell.
    std::string label {};                                  ///< Label for the new tab.
    FocusPolicy focus = FocusPolicy::Take;                 ///< Whether the new tab takes the focus.
};

/// `tab.create` reply.
struct CreateTabReply
{
    using ApiRecordTag = void;

    std::string window {}; ///< The window id.
    std::string tab {};    ///< The new tab's id.
    std::string pane {};   ///< The new tab's pane.
};

/// `pane.split` request.
struct SplitRequest
{
    using ApiRecordTag = void;

    std::string pane {};                              ///< The pane to split.
    SplitDirection direction = SplitDirection::Right; ///< Where the new pane goes.
    std::optional<uint8_t> ratioPercent {};           ///< Share of the old pane the new one gets, in percent.
    std::string cwd {};                               ///< Working directory; empty for the default.
    std::vector<std::string> command {};              ///< Program and arguments; empty for the shell.
    std::string label {};                             ///< Label for the new pane.
    FocusPolicy focus = FocusPolicy::Take;            ///< Whether the new pane takes the focus.
};

/// `pane.split` reply.
struct SplitReply
{
    using ApiRecordTag = void;

    std::string tab {};  ///< The tab id.
    std::string pane {}; ///< The new pane's id.
};

/// `pane.send` request.
struct SendRequest
{
    using ApiRecordTag = void;

    std::string pane {};            ///< The pane.
    std::string text {};            ///< The bytes to type into it.
    SendWait wait = SendWait::None; ///< What to wait for.
};

/// `pane.send` reply.
struct SendReply
{
    using ApiRecordTag = void;

    std::optional<BlockInfo> block {}; ///< The block the text started, when it waited for one.
};

/// `pane.capture` request.
struct CaptureRequest
{
    using ApiRecordTag = void;

    std::string pane {}; ///< The pane.
    int32_t scrollback =
        0; ///< 0 the page only, N history lines above it, -1 all of it; below -1 is `BadRequest`.
};

/// `pane.capture` reply.
struct CaptureReply
{
    using ApiRecordTag = void;

    std::string pane {};                     ///< The pane's id.
    std::string text {};                     ///< The captured text.
    Truncation truncated = Truncation::None; ///< `head` when the start was cut to fit the reply cap.
    Position cursor {};                      ///< The cursor.
    Size size {};                            ///< The page size.
};

/// `pane.close` request.
struct ClosePaneRequest
{
    using ApiRecordTag = void;

    std::string pane {}; ///< The pane to close.
};

/// `tab.close` request.
struct CloseTabRequest
{
    using ApiRecordTag = void;

    std::string tab {}; ///< The tab to close.
};

/// `label.set` request.
struct SetLabelRequest
{
    using ApiRecordTag = void;

    std::string object {}; ///< `<kind>:<target>`, e.g. `pane:logs`.
    std::string label {};  ///< The new label; empty clears it.
};

/// `block.list` request.
struct BlockListRequest
{
    using ApiRecordTag = void;

    std::string pane {}; ///< The pane.
    uint32_t limit = 20; ///< At most this many blocks, newest first.
};

/// `block.list` reply.
struct BlockListReply
{
    using ApiRecordTag = void;

    std::string pane {};              ///< The pane's id.
    std::vector<BlockInfo> blocks {}; ///< The blocks.
};

/// `block.read` request.
struct BlockReadRequest
{
    using ApiRecordTag = void;

    std::string block {};            ///< The block; empty for the last finished one in `pane`.
    std::string pane {};             ///< The pane, when `block` is empty.
    BlockPart part = BlockPart::All; ///< Which part to read.
};

/// `block.read` reply.
struct BlockReadReply
{
    using ApiRecordTag = void;

    BlockInfo block {};                      ///< The block.
    std::string prompt {};                   ///< Its prompt text.
    std::string output {};                   ///< Its output text.
    Truncation truncated = Truncation::None; ///< `head` when the start was cut to fit the reply cap.
};

/// `block.wait` request.
struct WaitRequest
{
    using ApiRecordTag = void;

    std::string block {}; ///< The block to wait for; empty for the next one to finish in `pane`.
    std::string pane {};  ///< The pane, when `block` is empty.
};

/// `block.wait` reply.
struct WaitReply
{
    using ApiRecordTag = void;

    std::optional<BlockInfo> block {}; ///< The finished block.
};

/// `events.subscribe` request.
struct SubscribeRequest
{
    using ApiRecordTag = void;

    std::vector<std::string> kinds {}; ///< Event names; empty for all.
    std::string pane {};               ///< Restrict to this pane; empty for all.
};

/// `events.subscribe` reply.
struct SubscribeReply
{
    using ApiRecordTag = void;

    uint32_t subscription = 0; ///< The subscription's id.
};

/// `events.unsubscribe` request.
struct UnsubscribeRequest
{
    using ApiRecordTag = void;

    uint32_t subscription = 0; ///< The subscription to end.
};

/// `api.list` request.
struct ApiListRequest
{
    using ApiRecordTag = void;
};

/// One method or event in `api.list`.
struct ApiEntry
{
    using ApiRecordTag = void;

    std::string name {};    ///< Its name.
    std::string summary {}; ///< A one-line summary.
};

/// `api.list` reply.
struct ApiListReply
{
    using ApiRecordTag = void;

    std::vector<ApiEntry> methods {}; ///< The methods.
    std::vector<ApiEntry> events {};  ///< The events.
};

/// `api.describe` request.
struct ApiDescribeRequest
{
    using ApiRecordTag = void;

    std::string name {}; ///< The method or event.
};

/// `api.describe` reply.
struct ApiDescription
{
    using ApiRecordTag = void;

    std::string name {};        ///< Its name.
    std::string summary {};     ///< A one-line summary.
    RawJson inputSchema {};     ///< JSON Schema of the request.
    RawJson outputSchema {};    ///< JSON Schema of the reply.
    RawJson requestExample {};  ///< An example request.
    RawJson responseExample {}; ///< An example reply.
};

// }}}

} // namespace vtcontrol
