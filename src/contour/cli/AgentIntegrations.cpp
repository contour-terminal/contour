// SPDX-License-Identifier: Apache-2.0
//
// Hook surfaces mapped here, verified online 2026-11-11:
//   Claude Code: https://code.claude.com/docs/en/hooks
//   Codex CLI:   https://learn.chatgpt.com/docs/hooks
// Both run a command hook with one JSON document on stdin carrying `hook_event_name`, and per event:
//   PreToolUse/PermissionRequest/PostToolUse: `tool_name`, `tool_input` (`file_path`, `command`, ...);
//   Notification (Claude): `message` and `notification_type`; Stop: `last_assistant_message`.
// Codex has Interrupt and no Notification. Codex runs only hooks the user has reviewed and trusted, so the
// user must trust the hooks `contour agent setup` installs once, inside Codex.
#include <contour/cli/AgentIntegrations.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <string>

using namespace std::string_view_literals;

namespace contour::cli
{

namespace
{
    using Object = glz::generic::object_t;

    /// @param node A JSON value.
    /// @param key A member name.
    /// @return The member, or null.
    [[nodiscard]] glz::generic const* memberOf(glz::generic const& node, std::string_view key) noexcept
    {
        auto const* object = node.get_if<Object>();
        if (object == nullptr)
            return nullptr;
        auto const member = object->find(key);
        return member != object->end() ? &member->second : nullptr;
    }

    /// @param node A JSON value.
    /// @param key A member name.
    /// @return The member's text when @p node is an object holding a string there; else empty.
    [[nodiscard]] std::string_view textOf(glz::generic const& node, std::string_view key) noexcept
    {
        auto const* member = memberOf(node, key);
        auto const* text = member != nullptr ? member->get_if<std::string>() : nullptr;
        return text != nullptr ? std::string_view { *text } : std::string_view {};
    }

    /// @param text Possibly many lines.
    /// @return Its first non-blank line, trimmed.
    [[nodiscard]] std::string firstLine(std::string_view text)
    {
        for (auto const line: std::views::split(text, '\n'))
        {
            if (auto const view = core::trim(std::string_view { line }); !view.empty())
                return std::string { view };
        }
        return {};
    }

    /// How a tool call reads: "<prefix><what it works on>".
    struct ToolPhrase
    {
        std::string_view tool;   ///< The tool's name.
        std::string_view prefix; ///< What goes before the target.
    };

    inline constexpr auto ToolPhrases = std::array {
        ToolPhrase { "Edit", "Editing " },      ToolPhrase { "Write", "Editing " },
        ToolPhrase { "MultiEdit", "Editing " }, ToolPhrase { "NotebookEdit", "Editing " },
        ToolPhrase { "Read", "Reading " },      ToolPhrase { "Bash", "Running: " },
    };

    /// The `tool_input` members that name what a tool works on, most telling first.
    inline constexpr auto ToolTargetKeys = std::array { "file_path"sv, "notebook_path"sv, "command"sv };

    /// @param payload A tool event's payload.
    /// @return What the tool works on (a path, a command), or empty.
    [[nodiscard]] std::string_view toolTarget(glz::generic const& payload) noexcept
    {
        auto const* input = memberOf(payload, "tool_input");
        if (input == nullptr)
            return {};
        for (auto const key: ToolTargetKeys)
            if (auto const target = textOf(*input, key); !target.empty())
                return target;
        return {};
    }

    [[nodiscard]] HookOutcome working(std::string message = {})
    {
        return StatusReport { .state = "working", .kind = {}, .message = std::move(message) };
    }

    HookOutcome onPromptSubmit(glz::generic const&)
    {
        return working();
    }

    HookOutcome onPreToolUse(glz::generic const& payload)
    {
        auto const tool = textOf(payload, "tool_name");
        auto const target = toolTarget(payload);
        auto const row = std::ranges::find(ToolPhrases, tool, &ToolPhrase::tool);
        if (row == ToolPhrases.end() || target.empty())
            return working(std::string { tool });
        return working(std::string { row->prefix } + std::string { target });
    }

    HookOutcome onPermissionRequest(glz::generic const& payload)
    {
        auto const tool = textOf(payload, "tool_name");
        auto const target = toolTarget(payload);
        auto message = std::string { tool };
        if (!target.empty())
            message += ": " + std::string { target };
        return StatusReport { .state = "blocked", .kind = "permission", .message = std::move(message) };
    }

    /// How a Notification reads, by its `notification_type`.
    struct NotificationRow
    {
        std::string_view type;
        std::string_view kind; ///< Empty for a notification that is not worth showing.
    };

    inline constexpr auto NotificationRows = std::array {
        NotificationRow { "permission_prompt", "permission" },
        NotificationRow { "agent_needs_input", "question" },
        NotificationRow { "elicitation_dialog", "question" },
        NotificationRow { "elicitation_url_dialog", "question" },
    };

    /// Only the notification types of NotificationRows are worth showing; idle_prompt and the rest are not.
    HookOutcome onNotification(glz::generic const& payload)
    {
        auto const type = textOf(payload, "notification_type");
        auto const row = std::ranges::find(NotificationRows, type, &NotificationRow::type);
        if (row == NotificationRows.end())
            return std::monostate {};
        return StatusReport { .state = "blocked",
                              .kind = std::string { row->kind },
                              .message = std::string { textOf(payload, "message") } };
    }

    HookOutcome onElicitation(glz::generic const& payload)
    {
        return StatusReport { .state = "blocked",
                              .kind = "question",
                              .message = std::string { textOf(payload, "message") } };
    }

    HookOutcome onStopFailure(glz::generic const&)
    {
        return StatusReport { .state = "error", .kind = {}, .message = {} };
    }

    HookOutcome onInterrupt(glz::generic const&)
    {
        return StatusReport { .state = "idle", .kind = {}, .message = {} };
    }

    HookOutcome onStop(glz::generic const& payload)
    {
        return StatusReport { .state = "done",
                              .kind = {},
                              .message = firstLine(textOf(payload, "last_assistant_message")) };
    }

    HookOutcome onSessionEnd(glz::generic const&)
    {
        return StatusClear {};
    }

    /// One hook event and what it means.
    struct EventRow
    {
        std::string_view event;
        HookOutcome (*map)(glz::generic const& payload);
    };

    inline constexpr auto ClaudeEvents = std::array {
        EventRow { "UserPromptSubmit", &onPromptSubmit },
        EventRow { "PreToolUse", &onPreToolUse },
        EventRow { "PermissionRequest", &onPermissionRequest },
        EventRow { "Notification", &onNotification },
        EventRow { "Elicitation", &onElicitation },
        EventRow { "PostToolUse", &onPromptSubmit },
        EventRow { "PostToolUseFailure", &onPromptSubmit },
        EventRow { "Stop", &onStop },
        EventRow { "StopFailure", &onStopFailure },
        EventRow { "SessionEnd", &onSessionEnd },
    };

    inline constexpr auto CodexEvents = std::array {
        EventRow { "UserPromptSubmit", &onPromptSubmit },
        EventRow { "PreToolUse", &onPreToolUse },
        EventRow { "PermissionRequest", &onPermissionRequest },
        EventRow { "PostToolUse", &onPromptSubmit },
        EventRow { "Stop", &onStop },
        EventRow { "Interrupt", &onInterrupt },
        EventRow { "SessionEnd", &onSessionEnd },
    };

    /// @param events An event table.
    /// @return Its events by name, for AgentIntegration::hookEvents.
    template <std::size_t N>
    [[nodiscard]] constexpr std::array<std::string_view, N> namesOf(std::array<EventRow, N> const& events)
    {
        auto names = std::array<std::string_view, N> {};
        std::ranges::transform(events, names.begin(), &EventRow::event);
        return names;
    }

    inline constexpr auto ClaudeEventNames = namesOf(ClaudeEvents);
    inline constexpr auto CodexEventNames = namesOf(CodexEvents);

    /// @param events An event table.
    /// @param payload A hook payload.
    /// @return What the row of the payload's event makes of it; nothing for an event without a row.
    HookOutcome mapEvent(std::span<EventRow const> events, glz::generic const& payload)
    {
        auto const event = textOf(payload, "hook_event_name");
        auto const row = std::ranges::find(events, event, &EventRow::event);
        return row != events.end() ? row->map(payload) : HookOutcome {};
    }

    HookOutcome mapClaude(glz::generic const& payload)
    {
        return mapEvent(ClaudeEvents, payload);
    }

    HookOutcome mapCodex(glz::generic const& payload)
    {
        return mapEvent(CodexEvents, payload);
    }

    inline constexpr auto Integrations = std::array {
        AgentIntegration { AgentId::ClaudeCode,
                           "claude-code",
                           "Claude Code",
                           std::span<std::string_view const> { ClaudeEventNames },
                           &mapClaude },
        AgentIntegration { AgentId::Codex,
                           "codex",
                           "Codex",
                           std::span<std::string_view const> { CodexEventNames },
                           &mapCodex },
    };
} // namespace

std::span<AgentIntegration const> agentIntegrations() noexcept
{
    return Integrations;
}

AgentIntegration const* findIntegration(std::string_view integrationId) noexcept
{
    auto const row = std::ranges::find(Integrations, integrationId, &AgentIntegration::integrationId);
    return row != Integrations.end() ? &*row : nullptr;
}

AgentIntegration const& integrationOf(AgentId id) noexcept
{
    return Integrations[static_cast<std::size_t>(id)];
}

} // namespace contour::cli
