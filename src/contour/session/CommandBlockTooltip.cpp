// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandBlockTooltip.hpp>

#include <vtbackend/shell/BlockStatusText.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <format>

namespace contour::session
{

std::vector<std::string> commandBlockHeaderLines(vtbackend::CommandBlockRecord const& record,
                                                 std::chrono::local_seconds startedAt,
                                                 std::chrono::system_clock::time_point now)
{
    auto lines = std::vector<std::string> {};
    if (record.state == vtbackend::CommandBlockState::Prompting)
        return lines;

    if (!record.commandLine.empty())
        lines.push_back(record.commandLine.display());
    if (auto status = vtbackend::blockStatusText(record, now); !status.empty())
        lines.push_back(std::move(status));
    if (!record.workingDirectory.path.empty())
        lines.push_back(vtbackend::sanitizeCommandLine(record.workingDirectory.path,
                                                       vtbackend::SanitizePurpose::Display));
    // A record from another process may name neither when its prompt nor when its command started.
    if (record.commandStartedAt || record.promptStartedAt)
        lines.push_back(std::format("Started {:%H:%M:%S}", startedAt));
    return lines;
}

} // namespace contour::session
