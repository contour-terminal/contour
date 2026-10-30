// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QObject>

#include <cstdint>
#include <format>
#include <map>
#include <string_view>

/// The words a permission question is answered in: whether the asked-for action may run, and how long that
/// answer stands. Their own namespace, not contour::session, because Q_ENUM_NS needs Q_NAMESPACE in the
/// namespace that declares the enums, and this keeps that meta-object to these two; contour::session
/// re-exports them below. QML names them as Permission.Allow, Permission.Session, ... once
/// registerPermissionQmlTypes() has run.
namespace contour::session::permission
{
Q_NAMESPACE

/// What the user answered a permission question with.
enum class PermissionDecision : uint8_t
{
    Deny = 0, ///< The action does not run.
    Allow,    ///< The action runs.
};
Q_ENUM_NS(PermissionDecision)

/// How long an answer to a permission question stands.
enum class PermissionMemory : uint8_t
{
    Once = 0,   ///< This answer only.
    Session,    ///< Until the session ends.
    Persistent, ///< Recorded where the asker's grants live.
};
Q_ENUM_NS(PermissionMemory)

/// How heavily a permission prompt's line is set.
enum class PromptLineWeight : uint8_t
{
    Regular = 0, ///< As the rest of the prompt.
    Strong,      ///< Set apart: what the reader must not miss.
};
Q_ENUM_NS(PromptLineWeight)

/// How a permission prompt's line is laid out.
enum class PromptLineStyle : uint8_t
{
    Prose = 0, ///< Wrapped to the dialog's width.
    Verbatim,  ///< One row, elided, never wrapped: text a program supplied, shown as it is.
};
Q_ENUM_NS(PromptLineStyle)

/// Whether a permission prompt lets Allow stand for this answer only.
enum class AllowOnce : uint8_t
{
    Offered = 0, ///< Allow may be given once.
    Withheld,    ///< Allow must be remembered: the box has to be ticked first.
};
Q_ENUM_NS(AllowOnce)

} // namespace contour::session::permission

namespace contour::session
{

using permission::AllowOnce;
using permission::PermissionDecision;
using permission::PermissionMemory;
using permission::PromptLineStyle;
using permission::PromptLineWeight;

/// A set of user-facing activities that are guarded behind a permission-check wall.
enum class GuardedRole : uint8_t
{
    ChangeFont = 0,
    CaptureBuffer,
    ShowHostWritableStatusLine,
    BigPaste,
    RerunCommand,
};

/// The answers the user asked to stand for the rest of the session, per guarded role.
using PermissionCache = std::map<GuardedRole, PermissionDecision>;

/// Registers the permission enums with QML as the uncreatable `Permission` of `Contour.Terminal` 1.0.
/// Idempotent: the application and the test runner may both call it.
void registerPermissionQmlTypes();

} // namespace contour::session

template <>
// NOLINTNEXTLINE(readability-identifier-naming)
struct std::formatter<contour::session::GuardedRole>: std::formatter<std::string_view>
{
    template <typename FormatContext>
    auto format(contour::session::GuardedRole value, FormatContext& ctx) const
    {
        std::string_view output;
        // clang-format off
        switch (value)
        {
            case contour::session::GuardedRole::ChangeFont: output = "Change Font"; break;
            case contour::session::GuardedRole::CaptureBuffer: output = "Capture Buffer"; break;
            case contour::session::GuardedRole::ShowHostWritableStatusLine:  output = "show Host Writable Statusline"; break;
            case contour::session::GuardedRole::BigPaste:  output = "paste large number of characters"; break;
            case contour::session::GuardedRole::RerunCommand: output = "run a command again"; break;
        }
        // clang-format on
        return formatter<string_view>::format(output, ctx);
    }
};

template <>
// NOLINTNEXTLINE(readability-identifier-naming)
struct std::formatter<contour::session::PermissionDecision>: std::formatter<std::string_view>
{
    auto format(contour::session::PermissionDecision value, auto& ctx) const
    {
        auto output = std::string_view {};
        switch (value)
        {
            case contour::session::PermissionDecision::Deny: output = "deny"; break;
            case contour::session::PermissionDecision::Allow: output = "allow"; break;
        }
        return formatter<string_view>::format(output, ctx);
    }
};

template <>
// NOLINTNEXTLINE(readability-identifier-naming)
struct std::formatter<contour::session::PermissionMemory>: std::formatter<std::string_view>
{
    auto format(contour::session::PermissionMemory value, auto& ctx) const
    {
        auto output = std::string_view {};
        switch (value)
        {
            case contour::session::PermissionMemory::Once: output = "once"; break;
            case contour::session::PermissionMemory::Session: output = "session"; break;
            case contour::session::PermissionMemory::Persistent: output = "persistent"; break;
        }
        return formatter<string_view>::format(output, ctx);
    }
};
