// SPDX-License-Identifier: Apache-2.0
#include <vthost/control/PaneText.hpp>

#include <vtbackend/screen/Terminal.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <vector>

#include <vtcontrol/Json.hpp>
#include <vtcontrol/ObjectRef.hpp>

namespace vthost::control
{

namespace
{
    [[nodiscard]] constexpr bool isContinuationByte(char ch) noexcept
    {
        return (static_cast<unsigned char>(ch) & 0xC0U) == 0x80U;
    }
} // namespace

BoundedText keepTail(std::string text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return BoundedText { .text = std::move(text), .truncated = vtcontrol::Truncation::None };
    auto start = text.size() - maxBytes;
    while (start < text.size() && isContinuationByte(text[start]))
        ++start;
    text.erase(0, start);
    return BoundedText { .text = std::move(text), .truncated = vtcontrol::Truncation::Head };
}

std::pair<BoundedText, BoundedText> boundBlockText(std::string prompt,
                                                   std::string output,
                                                   std::size_t maxBytes)
{
    if (prompt.size() >= maxBytes)
        return { keepTail(std::move(prompt), maxBytes),
                 BoundedText { .text = {},
                               .truncated = output.empty() ? vtcontrol::Truncation::None
                                                           : vtcontrol::Truncation::Head } };
    auto const promptBytes = prompt.size();
    return { BoundedText { .text = std::move(prompt), .truncated = vtcontrol::Truncation::None },
             keepTail(std::move(output), maxBytes - promptBytes) };
}

std::string captureText(vtbackend::Terminal const& terminal, int32_t scrollback)
{
    auto const& grid = terminal.currentScreen().grid();
    auto const history = unbox<int32_t>(grid.historyLineCount());
    auto const above = scrollback < 0 ? history : std::min(scrollback, history);
    auto const pageLines = unbox<int32_t>(grid.pageSize().lines);

    auto rows = std::vector<std::string> {};
    for (auto const line: std::views::iota(-above, pageLines))
        rows.push_back(grid.lineTextTrimmed(vtbackend::LineOffset(line)));
    while (!rows.empty() && rows.back().empty())
        rows.pop_back();

    auto text = std::string {};
    for (auto const& row: rows)
    {
        text += row;
        text += '\n';
    }
    if (!text.empty())
        text.pop_back();
    return text;
}

std::string formatTimestamp(std::chrono::system_clock::time_point instant)
{
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(instant));
}

vtcontrol::BlockState blockStateOf(vtbackend::CommandBlockRecord const& record) noexcept
{
    switch (record.state)
    {
        case vtbackend::CommandBlockState::Prompting: return vtcontrol::BlockState::Prompting;
        case vtbackend::CommandBlockState::Running: return vtcontrol::BlockState::Running;
        case vtbackend::CommandBlockState::Finished:
            return record.end == vtbackend::CommandBlockEnd::Implicit ? vtcontrol::BlockState::Superseded
                                                                      : vtcontrol::BlockState::Finished;
    }
    return vtcontrol::BlockState::Finished;
}

vtcontrol::BlockInfo blockInfoOf(vtcontrol::Uuid pane,
                                 vtbackend::CommandBlockRecord const& record,
                                 uint64_t epoch)
{
    auto info = vtcontrol::BlockInfo {};
    info.id = vtcontrol::toString(
        vtcontrol::BlockRef { .pane = pane, .serial = vtcontrol::blockSerial(epoch, record.id.value) });
    info.pane =
        vtcontrol::toString(vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = pane });
    info.state = blockStateOf(record);
    if (!record.commandLine.raw().empty())
        info.command = vtcontrol::toValidUtf8(record.commandLine.display());
    info.exitCode = record.exitCode;
    if (!record.workingDirectory.path.empty())
        info.cwd = vtcontrol::toValidUtf8(record.workingDirectory.path);
    if (record.promptStartedAt)
        info.promptAt = formatTimestamp(*record.promptStartedAt);
    if (record.commandStartedAt)
        info.startedAt = formatTimestamp(*record.commandStartedAt);
    if (record.duration)
    {
        auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(*record.duration).count();
        info.durationMs =
            static_cast<uint32_t>(std::clamp<int64_t>(millis, 0, std::numeric_limits<uint32_t>::max()));
    }
    return info;
}

} // namespace vthost::control
