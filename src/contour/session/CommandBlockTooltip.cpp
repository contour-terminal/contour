// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandBlockTooltip.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <string_view>

namespace contour::session
{

namespace
{
    /// The first line: how the command ended, or that it has not.
    [[nodiscard]] std::string statusLineOf(vtbackend::CommandBlockRecord const& record)
    {
        if (record.state == vtbackend::CommandBlockState::Running)
            return "Running";
        if (!record.exitCode)
            return "Finished, exit code unknown";
        auto const signal =
            vtbackend::contextSignalName(vtbackend::signalThatEnded(record.exitCode, record.outcome));
        if (signal.empty())
            return std::format("Exit code {}", *record.exitCode);
        return std::format("Exit code {} ({})", *record.exitCode, signal);
    }
} // namespace

std::string_view blockTooltipHintText(BlockTooltipHint hint) noexcept
{
    constexpr auto Texts = std::array {
        std::string_view { "Right-click for actions" },                     // RightClickOnly
        std::string_view { "Click for actions" },                           // Menu
        std::string_view { "Click to collapse · Right-click for actions" }, // Collapse
        std::string_view { "Click to expand · Right-click for actions" },   // Expand
    };
    static_assert(Texts.size() == static_cast<std::size_t>(BlockTooltipHint::Expand) + 1);
    return Texts[static_cast<std::size_t>(hint)];
}

std::string commandBlockTooltipText(vtbackend::CommandBlockRecord const& record,
                                    std::chrono::local_seconds startedAt)
{
    if (record.state == vtbackend::CommandBlockState::Prompting)
        return {};

    auto text = statusLineOf(record);
    if (record.duration)
        text += std::format("\nTook {}", vtbackend::formatCommandDuration(*record.duration));
    // A record from another process may name neither when its prompt nor when its command started; it then
    // has no start to show, rather than whatever @p startedAt was resolved from.
    if (record.commandStartedAt || record.promptStartedAt)
        text += std::format("\nStarted {:%Y-%m-%d %H:%M:%S}", startedAt);
    if (!record.workingDirectory.path.empty())
        text += std::format("\nIn {}",
                            vtbackend::sanitizeCommandLine(record.workingDirectory.path,
                                                           vtbackend::SanitizePurpose::Display));
    if (!record.commandLine.empty())
        text += std::format("\n$ {}", record.commandLine.display());
    return text;
}

} // namespace contour::session
