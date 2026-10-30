// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/BlockStatusText.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <format>
#include <ranges>
#include <vector>

namespace vtbackend
{

namespace
{
    /// The wording row of @p record: its ending's, or Running's.
    [[nodiscard]] BlockStatusWording const& wordingOf(CommandBlockRecord const& record) noexcept
    {
        if (record.state == CommandBlockState::Running)
            return BlockStatusWordingRows.back();
        auto const ending = commandEndingOf(record.exitCode, record.outcome);
        return BlockStatusWordingRows.at(static_cast<std::size_t>(ending));
    }

    /// How long @p record's command ran, or has been running at @p now; nullopt where nothing says.
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration> durationOf(
        CommandBlockRecord const& record, std::chrono::system_clock::time_point now)
    {
        // A running time ticks in whole seconds, the step it is redrawn at: "0s" in its first second, not
        // a count of milliseconds no redraw keeps up with (m4).
        if (record.state == CommandBlockState::Running && record.commandStartedAt)
            return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::floor<std::chrono::seconds>(now - *record.commandStartedAt));
        return record.duration;
    }

    /// The detail @p wording shows for @p record: `exit 8`, `SIGKILL`, or nothing.
    [[nodiscard]] std::string shownDetail(BlockStatusWording const& wording, CommandBlockRecord const& record)
    {
        auto text = finishDetailText(wording.detail, record.exitCode, record.outcome);
        if (text.empty())
            return text;
        return std::format(
            "{}{}", BlockStatusDetailPrefixes.at(static_cast<std::size_t>(wording.detail)), text);
    }

    /// The parts of @p record's status, in order: @p label, @p detail, and the duration as @p spellDuration
    /// spells it.
    template <typename Duration>
    [[nodiscard]] std::vector<std::string> statusParts(CommandBlockRecord const& record,
                                                       std::string_view label,
                                                       std::string detail,
                                                       std::chrono::system_clock::time_point now,
                                                       Duration const& spellDuration)
    {
        auto parts = std::vector<std::string> {};
        if (record.state == CommandBlockState::Prompting)
            return parts;
        if (!label.empty())
            parts.emplace_back(label);
        if (!detail.empty())
            parts.push_back(std::move(detail));
        if (auto const duration = durationOf(record, now))
            parts.push_back(spellDuration(*duration));
        return parts;
    }

    /// @p parts joined by @p separator.
    [[nodiscard]] std::string joined(std::vector<std::string> const& parts, std::string_view separator)
    {
        auto text = std::string {};
        for (auto const& part: parts)
        {
            if (!text.empty())
                text += separator;
            text += part;
        }
        return text;
    }

    /// A running command's time in its first second: whole seconds, as it is redrawn (m4).
    constexpr auto RunningFirstSecond = std::string_view { "0s" };

    /// @p count @p unit, its plural for any count but one.
    [[nodiscard]] std::string counted(long long count, std::string_view unit)
    {
        return std::format("{} {}{}", count, unit, count == 1 ? "" : "s");
    }
} // namespace

std::string spokenCommandDuration(std::chrono::steady_clock::duration duration)
{
    using namespace std::chrono;
    // formatCommandDuration()'s thresholds, its units spelled out: whole units below the coarsest, dropped.
    auto const clamped = std::max(duration, steady_clock::duration::zero());
    if (clamped < 1s)
        return counted(duration_cast<milliseconds>(clamped).count(), "millisecond");
    if (clamped < 1min)
        return counted(duration_cast<seconds>(clamped).count(), "second");
    if (clamped < 1h)
    {
        auto const total = duration_cast<seconds>(clamped).count();
        return std::format("{} {}", counted(total / 60, "minute"), counted(total % 60, "second"));
    }
    auto const minutes = duration_cast<std::chrono::minutes>(clamped).count();
    return std::format("{} {}", counted(minutes / 60, "hour"), counted(minutes % 60, "minute"));
}

std::string blockStatusText(CommandBlockRecord const& record, std::chrono::system_clock::time_point now)
{
    auto const& wording = wordingOf(record);
    return joined(statusParts(record,
                              wording.label,
                              shownDetail(wording, record),
                              now,
                              [&](auto duration) {
                                  // Whole seconds while running, the first one too: "0s", never "0ms".
                                  if (record.state == CommandBlockState::Running
                                      && duration < std::chrono::seconds(1))
                                      return std::string { RunningFirstSecond };
                                  return formatCommandDuration(duration);
                              }),
                  " · ");
}

std::string spokenBlockStatus(CommandBlockRecord const& record, std::chrono::system_clock::time_point now)
{
    auto const& wording = wordingOf(record);
    return joined(statusParts(record,
                              wording.spokenLabel,
                              shownDetail(wording, record),
                              now,
                              [&](auto duration) {
                                  if (record.state == CommandBlockState::Running
                                      && duration < std::chrono::seconds(1))
                                      return counted(0, "second");
                                  return spokenCommandDuration(duration);
                              }),
                  ", ");
}

BlockChipColors blockChipColors(ColorPalette const& palette, CommandBlockOutcome outcome) noexcept
{
    auto const success = outcome == CommandBlockOutcome::Success;
    auto colors = BlockChipColors {
        .text = success ? palette.gutterTextColor() : palette.blockStatusColor(outcome),
        .band = success ? std::nullopt : std::optional { palette.blockStatusColor(outcome) },
        .bandAlpha = BlockChipBandAlpha,
    };
    auto const ground = chipGround(palette, colors);
    colors.text = liftToContrast(colors.text, legibleInkOn(palette, ground), ground, MinimumTextContrast);
    return colors;
}

BlockChipColors foldChipColors(ColorPalette const& palette) noexcept
{
    auto colors = BlockChipColors { .text = palette.gutterTextColor(),
                                    .band = palette.gutterTextColor(),
                                    .bandAlpha = FoldChipBandAlpha };
    auto const ground = chipGround(palette, colors);
    colors.text = liftToContrast(colors.text, legibleInkOn(palette, ground), ground, MinimumTextContrast);
    return colors;
}

RGBColor chipGround(ColorPalette const& palette, BlockChipColors const& colors) noexcept
{
    return colors.band ? mixColor(palette.defaultBackground, *colors.band, colors.bandAlpha)
                       : palette.defaultBackground;
}

} // namespace vtbackend
