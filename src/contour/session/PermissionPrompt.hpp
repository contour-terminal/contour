// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/session/PastePreview.hpp>
#include <contour/session/Permission.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <QtCore/QVariant>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::session
{

/// One line of what a permission prompt says beneath its title.
struct PromptLine
{
    std::string text {}; ///< Plain text, already made safe to show.
    PromptLineWeight weight = PromptLineWeight::Regular;
    PromptLineStyle style = PromptLineStyle::Prose;

    bool operator==(PromptLine const&) const = default;
};

/// Everything a permission dialog shows, as data: its title names the action, its lines give the detail.
struct PermissionPrompt
{
    std::string title {};                                          ///< The question, naming the action.
    std::vector<PromptLine> lines {};                              ///< The detail beneath it.
    std::string denyText {};                                       ///< The Deny button's label.
    std::string allowText {};                                      ///< The Allow button's label.
    std::string rememberText {};                                   ///< The checkbox's label.
    PermissionMemory rememberScope = PermissionMemory::Session;    ///< How long a ticked box remembers.
    PermissionMemory initialMemory = PermissionMemory::Once;       ///< Whether the box starts ticked.
    PermissionDecision defaultDecision = PermissionDecision::Deny; ///< The button Return takes.
    AllowOnce allowOnce = AllowOnce::Offered;                      ///< Whether Allow needs the box ticked.

    bool operator==(PermissionPrompt const&) const = default;
};

/// What each guarded role's prompt offers, and which setting of the configuration answers it.
struct GuardedRoleRow
{
    GuardedRole role;
    /// The profile's setting for the role; nullptr for a role the configuration has no setting for.
    config::Permission config::PermissionsConfig::* setting;
    std::string_view denyText;
    std::string_view allowText;
    /// The button Return takes: Deny for what a program asks and for a rerun, whose command line any
    /// program can forge; Allow for a paste, which the user just asked for.
    PermissionDecision defaultDecision;
};

/// One row per guarded role, in enumerator order.
inline constexpr auto GuardedRoleRows = std::array {
    GuardedRoleRow { .role = GuardedRole::ChangeFont,
                     .setting = &config::PermissionsConfig::changeFont,
                     .denyText = "Deny",
                     .allowText = "Change Font",
                     .defaultDecision = PermissionDecision::Deny },
    GuardedRoleRow { .role = GuardedRole::CaptureBuffer,
                     .setting = &config::PermissionsConfig::captureBuffer,
                     .denyText = "Deny",
                     .allowText = "Allow",
                     .defaultDecision = PermissionDecision::Deny },
    GuardedRoleRow { .role = GuardedRole::ShowHostWritableStatusLine,
                     .setting = &config::PermissionsConfig::displayHostWritableStatusLine,
                     .denyText = "Deny",
                     .allowText = "Show",
                     .defaultDecision = PermissionDecision::Deny },
    // Gated on the paste's size, not on the configuration.
    GuardedRoleRow { .role = GuardedRole::BigPaste,
                     .setting = nullptr,
                     .denyText = "Cancel",
                     .allowText = "Paste",
                     .defaultDecision = PermissionDecision::Allow },
    // Asked every time unless the user said otherwise for the session: the line is the shell's report,
    // which any program writing to the terminal can forge, so it runs only once it has been read.
    GuardedRoleRow { .role = GuardedRole::RerunCommand,
                     .setting = nullptr,
                     .denyText = "Cancel",
                     .allowText = "Run",
                     .defaultDecision = PermissionDecision::Deny },
};

static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 0 }, GuardedRoleRows.size()))
            if (std::to_underlying(GuardedRoleRows[index].role) != index)
                return false;
        return true;
    }(),
    "GuardedRoleRows must list every guarded role in enumerator order");
static_assert(GuardedRoleRows.size() == std::to_underlying(GuardedRole::RerunCommand) + 1U,
              "GuardedRoleRows must have a row for every guarded role");

/// The row of @p role.
[[nodiscard]] constexpr GuardedRoleRow const& guardedRoleRow(GuardedRole role) noexcept
{
    return GuardedRoleRows[std::to_underlying(role)];
}

/// The permission @p permissions configure for @p role.
/// @param permissions The profile's permission settings.
/// @param role The guarded role.
/// @return Its setting; Ask for a role the configuration has no setting for.
[[nodiscard]] config::Permission configuredPermission(config::PermissionsConfig const& permissions,
                                                      GuardedRole role) noexcept;

/// The checkbox a guarded role's prompt offers.
inline constexpr std::string_view SessionRememberText = "Don't ask again in this session";

/// The prompt asking whether a program may change the font.
/// @param subject The session's title, as the program set it (made safe here).
/// @param font What the program asks for.
[[nodiscard]] PermissionPrompt fontChangePrompt(std::string_view subject, vtbackend::FontDef const& font);

/// The prompt asking whether a program may read the screen back.
/// @param subject The session's title, as the program set it (made safe here).
/// @param lines The most lines one of its captures asks for.
[[nodiscard]] PermissionPrompt bufferCapturePrompt(std::string_view subject, vtbackend::LineCount lines);

/// The prompt asking whether a program may show its own status line.
/// @param subject The session's title, as the program set it (made safe here).
[[nodiscard]] PermissionPrompt hostStatusLinePrompt(std::string_view subject);

/// The prompt asking whether a large paste goes ahead.
/// @param subject The session's title, as the program set it (made safe here).
/// @param bytes How large the paste is.
/// @param preview What it begins with.
[[nodiscard]] PermissionPrompt bigPastePrompt(std::string_view subject,
                                              std::size_t bytes,
                                              PastePreview const& preview);

/// The prompt asking whether a command runs again.
/// @param displayedCommandLine The command line, already made safe to show.
[[nodiscard]] PermissionPrompt rerunPrompt(std::string_view displayedCommandLine);

/// @p prompt as QML reads it (TerminalSession::pendingPermission()): `id`, `title`, `lines` (`{text, weight,
/// style}`), `denyText`, `allowText`, `rememberText`, `rememberScope`, `initialMemory`, `defaultDecision` and
/// `allowOnce`, every enum as its underlying int.
/// @param prompt The question.
/// @param questionId The id an answer to it names.
/// @return The map.
[[nodiscard]] QVariantMap permissionPromptMap(PermissionPrompt const& prompt, int questionId);

} // namespace contour::session
