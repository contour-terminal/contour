// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/EnumTable.hpp>

namespace vtcontrol
{

/// What a call needs to be granted.
enum class Permission : uint8_t
{
    Read = 0, ///< Look at the workspace.
    Screen,   ///< Read what is on screen and in command blocks.
    Input,    ///< Type into a pane.
    Write,    ///< Change the workspace.
};

template <>
struct EnumTable<Permission>
{
    static constexpr std::array Rows {
        EnumName { Permission::Read, "read" },
        EnumName { Permission::Screen, "screen" },
        EnumName { Permission::Input, "input" },
        EnumName { Permission::Write, "write" },
    };
};

/// What granting a permission lets an extension do, as its consent prompt says it: "May <phrase>.".
struct PermissionPhrase
{
    Permission permission;   ///< The permission.
    std::string_view phrase; ///< What it allows, lower-case, without the final full stop.
};

/// One row per `Permission`, in enumerator order, as `EnumTable<Permission>` has.
inline constexpr std::array PermissionPhrases {
    PermissionPhrase { .permission = Permission::Read, .phrase = "see the windows, tabs and panes" },
    PermissionPhrase { .permission = Permission::Screen,
                       .phrase = "read what panes show, and their command history" },
    PermissionPhrase { .permission = Permission::Input, .phrase = "type into panes" },
    PermissionPhrase { .permission = Permission::Write,
                       .phrase = "open, split, close and label tabs and panes" },
};

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: PermissionPhrases)
            if (static_cast<std::size_t>(row.permission) != index++)
                return false;
        return index == EnumTable<Permission>::Rows.size();
    }(),
    "PermissionPhrases holds one row per Permission, in enumerator order");

/// @param permission A permission.
/// @return What it allows, for a consent prompt ("type into panes").
[[nodiscard]] constexpr std::string_view consentPhraseOf(Permission permission) noexcept
{
    auto const index = static_cast<std::size_t>(permission);
    return index < PermissionPhrases.size() ? PermissionPhrases[index].phrase : std::string_view {};
}

/// The permissions a connection holds. Permissions genuinely combine -- a script's token may read the
/// screen yet never type -- so the set is a bitmask over `Permission`, not a pile of enums.
class ControlGrants
{
  public:
    /// @return The empty set.
    [[nodiscard]] static constexpr ControlGrants none() noexcept { return ControlGrants {}; }

    /// @return Every permission: what a local socket's connection holds.
    [[nodiscard]] static constexpr ControlGrants all() noexcept
    {
        auto grants = ControlGrants {};
        for (auto const& row: EnumTable<Permission>::Rows)
            grants = grants.with(row.value);
        return grants;
    }

    /// @param permissions The permissions to hold.
    /// @return The set of exactly those.
    [[nodiscard]] static constexpr ControlGrants of(std::initializer_list<Permission> permissions) noexcept
    {
        auto grants = ControlGrants {};
        for (auto const permission: permissions)
            grants = grants.with(permission);
        return grants;
    }

    /// @param permission A permission.
    /// @return Whether this set holds it.
    [[nodiscard]] constexpr bool contains(Permission permission) const noexcept
    {
        return (_bits & bitOf(permission)) != 0;
    }

    /// @param permission A permission.
    /// @return This set and @p permission.
    [[nodiscard]] constexpr ControlGrants with(Permission permission) const noexcept
    {
        auto grants = *this;
        grants._bits |= bitOf(permission);
        return grants;
    }

    [[nodiscard]] constexpr bool operator==(ControlGrants const&) const noexcept = default;

  private:
    [[nodiscard]] static constexpr uint8_t bitOf(Permission permission) noexcept
    {
        return static_cast<uint8_t>(1U << std::to_underlying(permission));
    }

    uint8_t _bits = 0;
};

/// @param commaList `all`, or a comma list of permission names (`read,screen`).
/// @return The grants it names, or `BadRequest` naming the first unknown name.
[[nodiscard]] std::expected<ControlGrants, ControlError> parseGrants(std::string_view commaList);

/// @param grants A set of permissions.
/// @return Their names as a comma list in enumerator order (`read,screen`), or `none` for the empty set.
[[nodiscard]] std::string toString(ControlGrants grants);

/// Who may call a method.
enum class Exposure : uint8_t
{
    Everywhere = 0, ///< Sockets and extensions alike.
    SocketOnly,     ///< Sockets only; an extension gets `Unsupported`.
};

/// A permission a request needs beyond its method's, for something it may ask the method to do.
/// @tparam Req The request type.
template <typename Req>
struct RequestGrantRule
{
    Permission permission = Permission::Read; ///< What must be granted as well.
    std::string_view reason {}; ///< What the request does that needs it, e.g. `running a command`.
    /// @return Whether @p request does what needs @ref permission.
    bool (*appliesTo)(Req const& request) noexcept = nullptr;
};

/// @return Whether @p request names a command to run.
/// @tparam Req A request with a `command` member.
template <typename Req>
[[nodiscard]] constexpr bool namesCommand(Req const& request) noexcept
{
    return !request.command.empty();
}

/// Running a program is typing it, so a request that names a command needs `input` beside the method's
/// `write`: a token that may rearrange the workspace may not run what it likes in it.
/// @tparam Req A request with a `command` member.
template <typename Req>
inline constexpr auto CommandNeedsInput = RequestGrantRule<Req> {
    .permission = Permission::Input,
    .reason = "running a command",
    .appliesTo = &namesCommand<Req>,
};

/// @return @p rule when @p request does what it guards, so needs its permission as well; otherwise nothing.
template <typename Req>
[[nodiscard]] constexpr std::optional<RequestGrantRule<Req>> requestGrantOf(
    std::optional<RequestGrantRule<Req>> const& rule, Req const& request) noexcept
{
    // A rule without a predicate never applies, rather than being called.
    if (rule && rule->appliesTo != nullptr && rule->appliesTo(request))
        return rule;
    return std::nullopt;
}

/// One row of the method catalog.
/// @tparam Req The request type.
/// @tparam Rep The reply type.
template <typename Req, typename Rep>
struct Method
{
    using Request = Req;
    using Reply = Rep;

    std::string_view name;    ///< The wire name, `noun.verb`.
    std::string_view summary; ///< A one-line summary.
    // The defaults are the restrictive cases, so that a row forgetting either is closed rather than open.
    Permission permission = Permission::Write; ///< What a connection must be granted.
    Exposure exposure = Exposure::SocketOnly;  ///< Who may call it.
    std::string_view requestExample;           ///< An example request; decodes as `Request`.
    std::string_view responseExample;          ///< An example reply; decodes as `Reply`.
    /// What a request may need granted beyond @ref permission, decided by what it asks for; nothing for a
    /// method whose requests never need more than it does.
    std::optional<RequestGrantRule<Req>> requestGrant = std::nullopt;
};

/// Every method of the control API, once.
inline constexpr auto Methods = std::tuple {
    Method<DaemonInfoRequest, DaemonInfoReply> {
        "daemon.info",
        "Describe the daemon: version, protocol, platform, host and uptime.",
        Permission::Read,
        Exposure::Everywhere,
        R"({})",
        R"({"version":"0.9.0","protocolVersion":1,"platform":"linux","hostname":"devbox","uptimeSeconds":3600})",
    },
    Method<ListRequest, ListReply> {
        "workspace.list",
        "List the windows, tabs and panes.",
        Permission::Read,
        Exposure::Everywhere,
        R"({"window":""})",
        R"({"windows":[{"id":"window:018f3c4e-9a1b-7c2d-8e3f-0123456789ab","label":"","position":1,"tabs":[{"id":"tab:018f3c4e-9a1b-7c2d-8e3f-0123456789ac","label":"build","title":"zsh","position":1,"isActive":true,"panes":[{"id":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad","label":"","title":"zsh","cwd":"/home/me","isActive":true,"size":{"columns":80,"lines":24},"programStatus":[]}]}]}]})",
    },
    Method<CreateTabRequest, CreateTabReply> {
        "tab.create",
        "Open a tab, optionally in a new window and running a command.",
        Permission::Write,
        Exposure::Everywhere,
        R"({"window":"","placement":"existing","cwd":"/tmp","command":["make","-j8"],"label":"build","focus":"keep"})",
        R"({"window":"window:018f3c4e-9a1b-7c2d-8e3f-0123456789ab","tab":"tab:018f3c4e-9a1b-7c2d-8e3f-0123456789ac","pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad"})",
        CommandNeedsInput<CreateTabRequest>,
    },
    Method<SplitRequest, SplitReply> {
        "pane.split",
        "Split a pane and start a shell or command in the new half.",
        Permission::Write,
        Exposure::Everywhere,
        R"({"pane":"logs","direction":"below","ratioPercent":30,"cwd":"","command":[],"label":"tail","focus":"keep"})",
        R"({"tab":"tab:018f3c4e-9a1b-7c2d-8e3f-0123456789ac","pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ae"})",
        CommandNeedsInput<SplitRequest>,
    },
    Method<SendRequest, SendReply> {
        "pane.send",
        "Type text into a pane, optionally waiting for the command it starts to finish.",
        Permission::Input,
        Exposure::Everywhere,
        R"({"pane":"build","text":"make test\n","wait":"commandFinished"})",
        R"({"block":{"id":"block:018f3c4e-9a1b-7c2d-8e3f-0123456789ad/4294967338","pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad","state":"finished","command":"make test","exitCode":0,"durationMs":1200}})",
    },
    Method<CaptureRequest, CaptureReply> {
        "pane.capture",
        "Read a pane's text: the page, and optionally history above it.",
        Permission::Screen,
        Exposure::Everywhere,
        R"({"pane":"build","scrollback":100})",
        R"({"pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad","text":"$ make\nok\n","truncated":"none","cursor":{"line":2,"column":2},"size":{"columns":80,"lines":24}})",
    },
    Method<ClosePaneRequest, EmptyReply> {
        "pane.close",
        "Close a pane and end its shell.",
        Permission::Write,
        Exposure::Everywhere,
        R"({"pane":"logs"})",
        R"({})",
    },
    Method<CloseTabRequest, EmptyReply> {
        "tab.close",
        "Close a tab with all its panes.",
        Permission::Write,
        Exposure::Everywhere,
        R"({"tab":"2"})",
        R"({})",
    },
    Method<SetLabelRequest, EmptyReply> {
        "object.setLabel",
        "Give a window, tab or pane a label to address it by; an empty label clears it.",
        Permission::Write,
        Exposure::Everywhere,
        R"({"object":"pane:logs","label":"tail"})",
        R"({})",
    },
    Method<BlockListRequest, BlockListReply> {
        "block.list",
        "List a pane's recent command blocks, newest first.",
        Permission::Screen,
        Exposure::Everywhere,
        R"({"pane":"build","limit":5})",
        R"({"pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad","blocks":[{"id":"block:018f3c4e-9a1b-7c2d-8e3f-0123456789ad/4294967338","pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad","state":"finished","command":"make","exitCode":0}]})",
    },
    Method<BlockReadRequest, BlockReadReply> {
        "block.read",
        "Read a command block's prompt and output; without a block, the last finished one of a pane.",
        Permission::Screen,
        Exposure::Everywhere,
        R"({"block":"","pane":"build","part":"output"})",
        R"({"block":{"id":"block:018f3c4e-9a1b-7c2d-8e3f-0123456789ad/4294967338","pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad","state":"finished","command":"make","exitCode":0},"prompt":"","output":"ok\n","truncated":"none"})",
    },
    Method<WaitRequest, WaitReply> {
        "block.wait",
        "Wait until a command block finishes and reply with it.",
        Permission::Screen,
        Exposure::SocketOnly,
        R"({"block":"","pane":"build"})",
        R"({"block":{"id":"block:018f3c4e-9a1b-7c2d-8e3f-0123456789ad/4294967338","pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad","state":"finished","exitCode":0}})",
    },
    Method<SubscribeRequest, SubscribeReply> {
        "events.subscribe",
        "Subscribe this connection to events.",
        Permission::Read,
        Exposure::SocketOnly,
        R"({"kinds":["block.finished","pane.bell"],"pane":""})",
        R"({"subscription":1})",
    },
    Method<UnsubscribeRequest, EmptyReply> {
        "events.unsubscribe",
        "End a subscription.",
        Permission::Read,
        Exposure::SocketOnly,
        R"({"subscription":1})",
        R"({})",
    },
    Method<ApiListRequest, ApiListReply> {
        "api.list",
        "List the methods and events of this API.",
        Permission::Read,
        Exposure::Everywhere,
        R"({})",
        R"({"methods":[{"name":"daemon.info","summary":"Describe the daemon."}],"events":[{"name":"pane.bell","summary":"A pane rang the bell."}]})",
    },
    Method<ApiDescribeRequest, ApiDescription> {
        "api.describe",
        "Describe one method or event: its schemas and examples.",
        Permission::Read,
        Exposure::Everywhere,
        R"({"name":"pane.split"})",
        R"({"name":"pane.split","summary":"Split a pane.","inputSchema":{},"outputSchema":{},"requestExample":{},"responseExample":{}})",
    },
};

} // namespace vtcontrol
