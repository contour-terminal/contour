// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Transcript.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Sanitize.hpp>
#include <orchestrator/view/LazyLines.hpp>
#include <orchestrator/view/Sections.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    /// How an entry of a kind is drawn: what its header calls it, and the roles of its header and its text.
    struct KindRow
    {
        TranscriptEntryKind kind = TranscriptEntryKind::User; ///< The kind.
        std::string_view label = {};                          ///< Its header's first word.
        Role header = Role::Text;                             ///< How its header is drawn.
        Role body = Role::Text;                               ///< How its text is drawn.
    };

    /// Every kind, in the order of the enumeration.
    constexpr auto KindRows = std::array {
        KindRow {
            .kind = TranscriptEntryKind::User, .label = "you"sv, .header = Role::Accent, .body = Role::Text },
        KindRow { .kind = TranscriptEntryKind::Assistant,
                  .label = "claude"sv,
                  .header = Role::Text,
                  .body = Role::Text },
        KindRow { .kind = TranscriptEntryKind::ToolCall,
                  .label = "tool"sv,
                  .header = Role::Dim,
                  .body = Role::Dim },
        KindRow { .kind = TranscriptEntryKind::ToolResult,
                  .label = "result"sv,
                  .header = Role::Dim,
                  .body = Role::Dim },
        KindRow { .kind = TranscriptEntryKind::ToolError,
                  .label = "error"sv,
                  .header = Role::Failing,
                  .body = Role::Failing },
        KindRow {
            .kind = TranscriptEntryKind::Note, .label = "note"sv, .header = Role::Note, .body = Role::Note },
    };

    // kindRowOf() indexes the table by the kind.
    static_assert(
        std::ranges::all_of(std::views::iota(std::size_t { 0 }, KindRows.size()), [](std::size_t index) {
            return static_cast<std::size_t>(std::to_underlying(KindRows[index].kind)) == index;
        }));

    /// @return How an entry of @p kind is drawn.
    [[nodiscard]] KindRow const& kindRowOf(TranscriptEntryKind kind) noexcept
    {
        auto const index = static_cast<std::size_t>(std::to_underlying(kind));
        return KindRows[index < KindRows.size() ? index : 0];
    }

    /// @return The line that ends a transcript cut at MaxTranscriptLines.
    [[nodiscard]] TranscriptLine ellipsisLine()
    {
        return TranscriptLine { .text = "…", .role = Role::Dim, .fit = DetailFit::Wrap };
    }

    /// The cells an entry's text is indented by under its header.
    constexpr auto BodyIndent = 2;

    /// The most characters of a tool's name a header shows.
    constexpr auto MaxToolNameChars = std::size_t { 40 };

    /// What a time that cannot be read is shown as.
    constexpr auto UnknownClock = "--:--"sv;

    constexpr auto MinutesPerDay = 24 * 60;

    /// What the screen says while the page is read.
    constexpr auto LoadingText = "reading the transcript…"sv;

    /// What the failed screen says instead of the command to read the page by hand, when a value in it would
    /// not be drawn as it is.
    constexpr auto UnshownCommandText =
        "The command to read it by hand cannot be shown safely: contour's path, the run's worktree path or "
        "its "
        "session holds characters that are drawn differently from what they are."sv;

    /// What the screen says once its run is gone: dismissed, here or in another tab.
    constexpr auto DismissedText = "This run was dismissed."sv;

    /// What the screen says while the configuration is refused: the engine reads nothing then.
    constexpr auto UnconfiguredText =
        "The transcript is not read while the configuration is refused; fix it to see the transcript."sv;

    /// What the screen says when contour and the orchestrator speak different formats.
    constexpr auto FormatMismatchText = "format mismatch: update contour or the orchestrator"sv;

    /// What a failed read says when the engine kept no reason.
    constexpr auto UnknownFailureText = "The transcript could not be read."sv;

    /// @return The digit @p c stands for; std::nullopt for anything else.
    [[nodiscard]] constexpr std::optional<int> digitOf(char c) noexcept
    {
        if (c < '0' || c > '9')
            return std::nullopt;
        return c - '0';
    }

    /// @return The two-digit number at @p at in @p text; std::nullopt when there is none.
    [[nodiscard]] constexpr std::optional<int> twoDigitsAt(std::string_view text, std::size_t at) noexcept
    {
        auto const tens = digitOf(text[at]);
        auto const ones = digitOf(text[at + 1]);
        if (!tens || !ones)
            return std::nullopt;
        return (*tens * 10) + *ones;
    }

    /// @return An entry's header and the text below it: a tool call names its tool in the header, its input
    ///         below.
    [[nodiscard]] std::pair<std::string, std::string_view> headerAndBodyOf(TranscriptEntry const& entry)
    {
        auto const& row = kindRowOf(entry.kind);
        auto label = std::string { row.label };
        auto body = std::string_view { entry.text };
        if (entry.kind == TranscriptEntryKind::ToolCall)
        {
            auto const space = body.find(' ');
            label = std::format("{} {}", row.label, displayText(body.substr(0, space), MaxToolNameChars));
            body = space == std::string_view::npos ? std::string_view {} : body.substr(space + 1);
        }
        return { std::format("{} · {}", label, entryClockOf(entry)), body };
    }
} // namespace

std::string localClockOf(std::string_view isoTime, int32_t utcOffsetMinutes)
{
    // `YYYY-MM-DDTHH:MM`: the time of day starts after the T.
    constexpr auto HourAt = std::size_t { 11 };
    constexpr auto MinuteAt = std::size_t { 14 };
    if (isoTime.size() < MinuteAt + 2 || isoTime[HourAt - 1] != 'T' || isoTime[MinuteAt - 1] != ':')
        return std::string { UnknownClock };
    auto const hours = twoDigitsAt(isoTime, HourAt);
    auto const minutes = twoDigitsAt(isoTime, MinuteAt);
    if (!hours || !minutes || *hours > 23 || *minutes > 59)
        return std::string { UnknownClock };
    auto const offset = std::clamp(utcOffsetMinutes, -MaxUtcOffsetMinutes, MaxUtcOffsetMinutes);
    auto const local =
        ((((*hours * 60) + *minutes + offset) % MinutesPerDay) + MinutesPerDay) % MinutesPerDay;
    return std::format("{:02}:{:02}", local / 60, local % 60);
}

std::string entryClockOf(TranscriptEntry const& entry)
{
    // Without the offset its instant had, the time is not guessed from today's: it is shown in UTC, so named.
    auto clock = localClockOf(entry.time, entry.utcOffsetMinutes.value_or(0));
    if (entry.utcOffsetMinutes || clock == UnknownClock)
        return clock;
    return std::format("{} UTC", clock);
}

std::vector<TranscriptLine> entryLines(TranscriptEntry const& entry, EntryPlace place, int columns)
{
    auto lines = std::vector<TranscriptLine> {};
    if (columns <= 0)
        return lines;
    if (place == EntryPlace::Later)
        lines.emplace_back();
    auto const& row = kindRowOf(entry.kind);
    auto const [header, body] = headerAndBodyOf(entry);
    lines.push_back(TranscriptLine { .text = cut(header, columns), .role = row.header });
    auto const indent = std::string(static_cast<std::size_t>(std::min(BodyIndent, columns / 2)), ' ');
    for (auto& text: indentedLines(
             body, columns - static_cast<int>(indent.size()), MaxTranscriptLines, MaxTranscriptLineChars))
        lines.push_back(TranscriptLine { .text = text.empty() ? std::string {} : indent + std::move(text),
                                         .role = row.body });
    return lines;
}

std::string transcriptFooterOf(TranscriptPage const& page)
{
    auto parts = std::vector<std::string> {};
    if (!page.writtenBy.empty())
        parts.push_back(std::format("written by Claude Code {}", page.writtenBy));
    if (page.notUnderstood > 0)
        parts.push_back(std::format(
            "{} {} not understood", page.notUnderstood, page.notUnderstood == 1 ? "entry" : "entries"));
    auto footer = std::string {};
    for (auto const& part: parts)
        footer += footer.empty() ? part : std::format("{}{}", HintSeparator, part);
    return footer;
}

std::optional<std::vector<std::string>> transcriptCommandOf(Run const& run,
                                                            uint32_t page,
                                                            std::span<HostProgram const> selfPrograms)
{
    // The contour the host runs the command with, which a PATH need not hold.
    constexpr auto Program = "contour"sv;
    auto const self = std::ranges::find(selfPrograms, Program, &HostProgram::name);
    auto const program = self != selfPrograms.end() ? std::string { self->path } : std::string { Program };
    // What is drawn could otherwise name another program, session or directory than the one the copy names.
    if (!showsAsItIs(program) || !showsAsItIs(run.session) || !showsAsItIs(run.worktree))
        return std::nullopt;
    auto groups = std::vector<std::vector<std::string>> {
        { program, "agent", "transcript" },
        { "--session", run.session },
        { "--cwd", run.worktree },
    };
    if (page > 1)
        groups.push_back({ "--page", std::format("{}", page) });
    auto lines = std::vector<std::string> {};
    for (auto const& group: groups)
    {
        if (!lines.empty())
            lines.back() += " \\";
        lines.push_back(shellQuoted(group));
    }
    return lines;
}

std::optional<std::vector<TranscriptLine>> transcriptScreenText(Engine const& engine,
                                                                std::string_view tab,
                                                                std::string_view run,
                                                                uint32_t page)
{
    auto const plain = [](std::string_view text, Role role = Role::Dim) {
        return std::optional { std::vector {
            TranscriptLine { .text = std::string { text }, .role = role } } };
    };
    // The engine reads no transcript of a run it no longer knows, nor any while its configuration is refused.
    auto const runs = engine.runs();
    auto const found = std::ranges::find(runs, run, &Run::id);
    if (found == runs.end())
        return plain(DismissedText);
    if (engine.config() == nullptr)
        return plain(UnconfiguredText);
    auto const* const view = engine.transcript(tab, run);
    if (view == nullptr || view->page != page)
        return plain(LoadingText);
    switch (view->state)
    {
        case FetchState::Loading: return plain(LoadingText);
        case FetchState::Loaded: return std::nullopt;
        case FetchState::FormatMismatch: return plain(FormatMismatchText, Role::Text);
        case FetchState::Failed: break;
    }
    // A failed read says why, and how to read the page by hand.
    auto lines = std::vector {
        TranscriptLine { .text = view->failure.empty() ? std::string { UnknownFailureText } : view->failure,
                         .role = Role::Failing },
        TranscriptLine {},
    };
    auto command = transcriptCommandOf(*found, page, engine.host().selfPrograms);
    if (!command)
    {
        lines.push_back(TranscriptLine { .text = std::string { UnshownCommandText }, .role = Role::Warning });
        return lines;
    }
    lines.push_back(TranscriptLine { .text = "To read it by hand, run:", .role = Role::Dim });
    for (auto& line: *command)
        lines.push_back(
            TranscriptLine { .text = std::move(line), .role = Role::Text, .fit = DetailFit::Whole });
    return lines;
}

TranscriptPanel::TranscriptPanel(Engine const& engine,
                                 Interaction const& interaction,
                                 std::string const& tab,
                                 TranscriptPanelState& state):
    _engine { engine },
    _interaction { interaction },
    _tab { tab },
    _state { state },
    _wrapped { MaxTranscriptLines, ellipsisLine() }
{
}

std::span<TranscriptLine const> TranscriptPanel::linesFor(TranscriptPage const& page,
                                                          int columns,
                                                          std::size_t count)
{
    // A new page, or a new width, is wrapped anew; as far as a draw asks.
    if (columns != _wrappedColumns || _wrappedPage != page)
    {
        _wrappedPage = page;
        _wrappedColumns = columns;
        _wrapped.reset(columns > 0 ? page.entries.size() : 0);
    }
    auto const lines = _wrapped.through(count, [&](std::size_t entry) {
        return entryLines(
            _wrappedPage->entries[entry], entry == 0 ? EntryPlace::First : EntryPlace::Later, columns);
    });
    _state.wrappedEntries = _wrapped.wrappedBlocks();
    return lines;
}

void TranscriptPanel::render(core::tui::Canvas& canvas)
{
    // The Narrow band shows the transcript alone, without the summary line that hides the cursor elsewhere.
    canvas.hideCursor();
    auto const& theme = canvas.theme();
    auto const* const view = _engine.transcript(_tab, _interaction.subject);
    auto const pages = view != nullptr && view->content ? view->content->pages : 0;
    // The run's id gives way first: the page is what the keys change.
    auto const where = pages > 0 ? std::format("page {} of {}", _interaction.page, pages)
                                 : std::format("page {}", _interaction.page);
    auto const titleColumns = canvas.width() - 6;
    auto const runColumns = titleColumns - columnsOf(std::format("Transcript ·  · {}", where));
    drawFrame(canvas, std::format("Transcript · {} · {}", cut(_interaction.subject, runColumns), where));
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - 2;
    auto const height = std::max(0, inside.height());
    _state = TranscriptPanelState { .lines = 0, .rows = 0, .wrappedEntries = _wrapped.wrappedBlocks() };

    if (auto const text = transcriptScreenText(_engine, _tab, _interaction.subject, _interaction.page);
        text || view == nullptr || !view->content)
    {
        auto line = 0;
        for (auto const& [said, role, fit]: text.value_or(std::vector<TranscriptLine> {}))
        {
            if (said.empty())
                ++line;
            // A command line is shown whole on its line, or replaced by a line that asks for more width:
            // never cut, wrapped or broken, so that what is copied is what runs.
            for (auto const& piece: fitted(said, fit, width, width))
                if (line < height)
                    inside.putString(line++, 1, piece, styleOf(theme, role));
        }
        return;
    }

    auto const& page = *view->content;
    auto const footer = transcriptFooterOf(page);
    auto const footerRows = footer.empty() ? 0 : 1;
    auto const rows = static_cast<std::size_t>(std::max(0, height - footerRows));
    // Wrapped as far as the lines shown: what lies below them is wrapped once it is scrolled to.
    auto const lines = linesFor(page, width, _interaction.scroll + rows);
    _state.lines = _wrapped.lineCount();
    _state.rows = rows;
    if (footerRows > 0 && height > 0)
        inside.putString(height - 1, 1, cut(footer, width), styleOf(theme, Role::Dim));
    if (lines.empty())
    {
        inside.putString(0, 1, cut("The transcript has no entries yet.", width), styleOf(theme, Role::Dim));
        return;
    }
    // The keys keep the scroll within the page as last drawn; one that shrank since is shown from its end.
    auto const top = std::min(_interaction.scroll, lines.size() > rows ? lines.size() - rows : 0);
    auto line = 0;
    for (auto const& [text, role, fit]: lines | std::views::drop(top) | std::views::take(rows))
        inside.putString(line++, 1, text, styleOf(theme, role));
}

} // namespace orchestrator::view
