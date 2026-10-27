// SPDX-License-Identifier: Apache-2.0
#include <contour/session/PermissionPrompt.hpp>

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <format>
#include <utility>

namespace contour::session
{

namespace
{
    /// The most bytes of the session's title a prompt's title quotes, before its ellipsis.
    constexpr auto SubjectBytes = std::size_t { 64 };

    /// How a prompt's title names the session: its title, made safe, cut and quoted -- or, without one,
    /// "this terminal".
    [[nodiscard]] std::string subjectPhrase(std::string_view subject)
    {
        auto const sanitized = vtbackend::sanitizeCommandLine(subject, vtbackend::SanitizePurpose::Display);
        if (sanitized.empty())
            return "this terminal";
        auto const shown = detail::cutAtCharacter(sanitized, SubjectBytes);
        return std::format("“{}{}”", shown, shown.size() < sanitized.size() ? detail::Ellipsis : "");
    }

    /// A guarded role's prompt, with its row's buttons and default, and the session-long checkbox.
    [[nodiscard]] PermissionPrompt guardedPrompt(GuardedRole role,
                                                 std::string title,
                                                 std::vector<PromptLine> lines)
    {
        auto const& row = guardedRoleRow(role);
        return PermissionPrompt { .title = std::move(title),
                                  .lines = std::move(lines),
                                  .denyText = std::string { row.denyText },
                                  .allowText = std::string { row.allowText },
                                  .rememberText = std::string { SessionRememberText },
                                  .rememberScope = PermissionMemory::Session,
                                  .initialMemory = PermissionMemory::Once,
                                  .defaultDecision = row.defaultDecision,
                                  .allowOnce = AllowOnce::Offered };
    }

    [[nodiscard]] PromptLine prose(std::string text)
    {
        return PromptLine { .text = std::move(text),
                            .weight = PromptLineWeight::Regular,
                            .style = PromptLineStyle::Prose };
    }

    [[nodiscard]] PromptLine verbatim(std::string text)
    {
        return PromptLine { .text = std::move(text),
                            .weight = PromptLineWeight::Regular,
                            .style = PromptLineStyle::Verbatim };
    }

    /// The font a request asks for, in the parts it sets: "{regular}, {size} pt".
    [[nodiscard]] std::string fontDescription(vtbackend::FontDef const& font)
    {
        auto const regular =
            vtbackend::sanitizeCommandLine(font.regular, vtbackend::SanitizePurpose::Display);
        auto const size = font.size != 0.0 ? std::format("{} pt", font.size) : std::string {};
        if (!regular.empty() && !size.empty())
            return std::format("{}, {}", regular, size);
        return regular.empty() ? size : regular;
    }
} // namespace

config::Permission configuredPermission(config::PermissionsConfig const& permissions,
                                        GuardedRole role) noexcept
{
    auto const& row = guardedRoleRow(role);
    return row.setting != nullptr ? permissions.*row.setting : config::Permission::Ask;
}

PermissionPrompt fontChangePrompt(std::string_view subject, vtbackend::FontDef const& font)
{
    auto description = fontDescription(font);
    return guardedPrompt(GuardedRole::ChangeFont,
                         std::format("Allow {} to change the font?", subjectPhrase(subject)),
                         { prose(description.empty() ? std::string { "It asks for another font." }
                                                     : std::move(description)) });
}

PermissionPrompt bufferCapturePrompt(std::string_view subject, vtbackend::LineCount lines)
{
    return guardedPrompt(
        GuardedRole::CaptureBuffer,
        std::format("Allow {} to read the screen?", subjectPhrase(subject)),
        { prose(std::format("It asks for the last {} lines of the terminal, to read them back.",
                            unbox(lines))) });
}

PermissionPrompt hostStatusLinePrompt(std::string_view subject)
{
    return guardedPrompt(GuardedRole::ShowHostWritableStatusLine,
                         std::format("Allow {} to show its own status line?", subjectPhrase(subject)),
                         { prose("It writes the text of the line below the terminal.") });
}

PermissionPrompt bigPastePrompt(std::string_view subject, std::size_t bytes, PastePreview const& preview)
{
    auto lines = std::vector<PromptLine> {};
    lines.reserve(preview.lines.size() + 1);
    for (auto const& line: preview.lines)
        lines.push_back(verbatim(line));
    if (preview.moreLines != 0)
        lines.push_back(prose(std::format("{} {} more lines", detail::Ellipsis, preview.moreLines)));
    return guardedPrompt(GuardedRole::BigPaste,
                         std::format("Paste {} into {}?", formatByteCount(bytes), subjectPhrase(subject)),
                         std::move(lines));
}

PermissionPrompt rerunPrompt(std::string_view displayedCommandLine)
{
    return guardedPrompt(
        GuardedRole::RerunCommand,
        "Run this command again?",
        { verbatim(std::string { displayedCommandLine }), prose("It is typed at the prompt and entered.") });
}

PromptLine consentLine(std::string text, vtcontrol::ConsentWeight weight)
{
    return PromptLine { .text = std::move(text),
                        .weight = weight == vtcontrol::ConsentWeight::CanRunCommands
                                      ? PromptLineWeight::Strong
                                      : PromptLineWeight::Regular,
                        .style = PromptLineStyle::Prose };
}

PermissionPrompt consentPrompt(std::string title, std::vector<PromptLine> lines)
{
    return PermissionPrompt { .title = std::move(title),
                              .lines = std::move(lines),
                              .denyText = "Deny",
                              .allowText = "Allow",
                              .rememberText = std::string { ExtensionRememberText },
                              .rememberScope = PermissionMemory::Persistent,
                              .initialMemory = PermissionMemory::Persistent,
                              .defaultDecision = PermissionDecision::Deny,
                              .allowOnce = AllowOnce::Withheld };
}

std::expected<ConsentVerdict, ConsentAnswerError> consentVerdictFor(PermissionDecision decision,
                                                                    PermissionMemory memory) noexcept
{
    switch (memory)
    {
        case PermissionMemory::Persistent:
            return decision == PermissionDecision::Allow ? ConsentVerdict::Allow : ConsentVerdict::Deny;
        case PermissionMemory::Once:
            if (decision == PermissionDecision::Allow)
                return std::unexpected { ConsentAnswerError::AllowMustBeRemembered };
            return ConsentVerdict::Dismiss;
        case PermissionMemory::Session: break;
    }
    return std::unexpected { ConsentAnswerError::ScopeNotOffered };
}

QVariantMap permissionPromptMap(PermissionPrompt const& prompt, int questionId)
{
    auto lines = QVariantList {};
    lines.reserve(static_cast<qsizetype>(prompt.lines.size()));
    for (auto const& line: prompt.lines)
        lines.append(QVariantMap {
            { QStringLiteral("text"), QString::fromStdString(line.text) },
            { QStringLiteral("weight"), static_cast<int>(std::to_underlying(line.weight)) },
            { QStringLiteral("style"), static_cast<int>(std::to_underlying(line.style)) },
        });
    return QVariantMap {
        { QStringLiteral("id"), questionId },
        { QStringLiteral("title"), QString::fromStdString(prompt.title) },
        { QStringLiteral("lines"), lines },
        { QStringLiteral("denyText"), QString::fromStdString(prompt.denyText) },
        { QStringLiteral("allowText"), QString::fromStdString(prompt.allowText) },
        { QStringLiteral("rememberText"), QString::fromStdString(prompt.rememberText) },
        { QStringLiteral("rememberScope"), static_cast<int>(std::to_underlying(prompt.rememberScope)) },
        { QStringLiteral("initialMemory"), static_cast<int>(std::to_underlying(prompt.initialMemory)) },
        { QStringLiteral("defaultDecision"), static_cast<int>(std::to_underlying(prompt.defaultDecision)) },
        { QStringLiteral("allowOnce"), static_cast<int>(std::to_underlying(prompt.allowOnce)) },
    };
}

} // namespace contour::session
