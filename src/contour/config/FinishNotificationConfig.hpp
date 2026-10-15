// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>

namespace contour::config
{

/// Which sessions a finished command is worth telling the user about, by where the session is.
enum class NotifyWhen : uint8_t
{
    Never = 0, ///< Never.
    Unfocused, ///< Unless the session is the focused one of the active window (the default).
    Hidden,    ///< Only while the session is off screen: another tab, a zoomed sibling, a minimised window.
    Always,    ///< Every time.
};

/// Which finished commands are worth telling the user about, by how they ended.
enum class NotifyOutcome : uint8_t
{
    Any = 0, ///< Every command.
    Failure, ///< Only failures: a non-zero exit, OSC 3008 `exit=failure`, a signal, a crash or an interrupt.
};

/// How the user is told.
enum class NotifyAction : uint8_t
{
    Notify = 0, ///< A desktop notification.
    Bell,       ///< The profile's bell sound and window alert.
    NotifyBell, ///< Both.
};

/// When a finish notification still on the desktop is withdrawn.
enum class NotifyClearOn : uint8_t
{
    Focus = 0, ///< When its session gains terminal focus, or its window comes back while it holds it.
    Next,      ///< When its session's next command starts.
    Never,     ///< Never; the desktop retires it.
};

/// The profile's `notify_on_command_finish` policy.
///
/// On by default (`unfocused`, ten seconds): the case it serves -- a build in a background tab -- is the
/// one nobody thinks to turn it on for, and the floor keeps ordinary commands quiet. Every member is
/// already an enum or a duration, so this YAML-facing struct carries no `bool` at all.
/// @see platform::finishNoticeKind
struct FinishNotificationConfig
{
    NotifyWhen when = NotifyWhen::Unfocused;      ///< Which sessions notify, by visibility.
    std::chrono::seconds minDuration { 10 };      ///< Commands that ran shorter never notify.
    NotifyOutcome outcome = NotifyOutcome::Any;   ///< Which outcomes notify.
    NotifyAction action = NotifyAction::Notify;   ///< How the user is told.
    NotifyClearOn clearOn = NotifyClearOn::Focus; ///< When a notification is withdrawn.

    bool operator==(FinishNotificationConfig const&) const = default;
};

namespace detail
{
    // inline, so every translation unit shares one table rather than a private copy the returned span
    // would point into. The reader, the writer and the settings page all read these rows.
    inline constexpr auto NotifyWhenTable = std::array {
        ConfigEnumInfo<NotifyWhen> { NotifyWhen::Never, "never", "Never" },
        ConfigEnumInfo<NotifyWhen> { NotifyWhen::Unfocused, "unfocused", "When the pane is not focused" },
        ConfigEnumInfo<NotifyWhen> { NotifyWhen::Hidden, "hidden", "When the pane is not on screen" },
        ConfigEnumInfo<NotifyWhen> { NotifyWhen::Always, "always", "Always" },
    };

    inline constexpr auto NotifyOutcomeTable = std::array {
        ConfigEnumInfo<NotifyOutcome> { NotifyOutcome::Any, "any", "Every command" },
        ConfigEnumInfo<NotifyOutcome> { NotifyOutcome::Failure, "failure", "Only failed commands" },
    };

    inline constexpr auto NotifyActionTable = std::array {
        ConfigEnumInfo<NotifyAction> { NotifyAction::Notify, "notify", "Desktop notification" },
        ConfigEnumInfo<NotifyAction> { NotifyAction::Bell, "bell", "Bell" },
        ConfigEnumInfo<NotifyAction> { NotifyAction::NotifyBell, "notify_bell", "Notification and bell" },
    };

    inline constexpr auto NotifyClearOnTable = std::array {
        ConfigEnumInfo<NotifyClearOn> { NotifyClearOn::Focus, "focus", "When the pane is focused" },
        ConfigEnumInfo<NotifyClearOn> { NotifyClearOn::Next, "next", "When the next command starts" },
        ConfigEnumInfo<NotifyClearOn> { NotifyClearOn::Never, "never", "Never" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<NotifyWhen> const> configEnumValues() noexcept
{
    return detail::NotifyWhenTable;
}

template <>
constexpr std::span<ConfigEnumInfo<NotifyOutcome> const> configEnumValues() noexcept
{
    return detail::NotifyOutcomeTable;
}

template <>
constexpr std::span<ConfigEnumInfo<NotifyAction> const> configEnumValues() noexcept
{
    return detail::NotifyActionTable;
}

template <>
constexpr std::span<ConfigEnumInfo<NotifyClearOn> const> configEnumValues() noexcept
{
    return detail::NotifyClearOnTable;
}

} // namespace contour::config
