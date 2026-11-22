// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Focus.hpp>

#include <core/tui/Rect.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Transcript.hpp>
#include <orchestrator/view/Actions.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    using core::tui::Canvas;
    using core::tui::Rect;

    /// The cells a detail's label takes, its padding included.
    constexpr auto LabelColumns = 10;
    /// How far a detail's continuation lines are indented past its value.
    constexpr auto ContinuationIndent = 2;
    /// The rows a box with one line of text takes.
    constexpr auto OneLineBoxRows = 3;
    /// What ends the message box's draft while it has the keys.
    constexpr auto Caret = "▏"sv;
    /// The cells between two reply chips.
    constexpr auto ChipGap = 3;
    /// The cells of a box's width its chips do not take: its border, its padding, and one spare.
    constexpr auto ChipInset = 5;

    /// @return The cells @p reply's chip takes: its digit, a space, its label.
    [[nodiscard]] int chipColumnsOf(QuickReply const& reply)
    {
        return 2 + columnsOf(reply.label);
    }

    /// One piece of furniture that gives way, and the rows it gives.
    struct GivingWay
    {
        Furniture item = Furniture::None; ///< The furniture.
        int rows = 0;                     ///< The rows it takes; for the top bar, FocusRows::topBarRows.
    };

    /// The furniture, in the order it gives way.
    constexpr auto GivingWayOrder = std::array {
        GivingWay { .item = Furniture::HeaderDetail, .rows = 1 },
        GivingWay { .item = Furniture::Latest, .rows = LatestRows },
        GivingWay { .item = Furniture::TopBar, .rows = 0 },
        GivingWay { .item = Furniture::TabStrip, .rows = 1 },
    };

    /// The label of each tab in the strip, and the key that shows it.
    struct TabLabel
    {
        FocusTab tab = FocusTab::Board;            ///< The tab.
        KeyCommand command = KeyCommand::BoardTab; ///< What shows it.
        std::string_view label = {};               ///< Its label; its first letter is its key.
        std::string_view word = {};                ///< What the header says when the strip gave way.
    };

    constexpr auto TabLabels = std::array {
        TabLabel {
            .tab = FocusTab::Board, .command = KeyCommand::BoardTab, .label = "Board"sv, .word = "board"sv },
        TabLabel { .tab = FocusTab::Transcript,
                   .command = KeyCommand::TranscriptTab,
                   .label = "Transcript"sv,
                   .word = "transcript"sv },
        TabLabel { .tab = FocusTab::Details,
                   .command = KeyCommand::DetailsTab,
                   .label = "Details"sv,
                   .word = "details"sv },
    };

    /// The primer the Focus shows while the Fleet is empty.
    constexpr auto Primer = std::array {
        "No agents yet. ⏎ or n starts one on a pull request, an issue, or anything you type."sv,
        "Once agents run: ⏎ talks to the selected one, 1 2 3 answers one that asks, esc steps back."sv,
        "Everything else is in ^K."sv,
    };

    /// One drawn line of the details: a detail's first line with its label, or one of its continuations.
    struct DetailText
    {
        std::string_view label = {}; ///< The detail's label on its first line; empty on a continuation.
        std::string text = {};       ///< Sanitised and wrapped to its cells.
        Role role = Role::Text;      ///< How it is drawn.
        int indent = 0;              ///< The cells it starts past the value column.
        std::string_view link = {};  ///< The URL it links to; empty: none.
    };

    /// @return The details fitted to @p columns cells as each says (fitted()), continuation lines indented.
    [[nodiscard]] std::vector<DetailText> detailLinesOf(std::span<DetailLine const> details, int columns)
    {
        auto lines = std::vector<DetailText> {};
        for (auto const& detail: details)
        {
            auto wrapped = fitted(detail.value, detail.fit, columns, columns - ContinuationIndent);
            if (wrapped.empty())
                wrapped.emplace_back();
            for (auto const index: std::views::iota(std::size_t { 0 }, wrapped.size()))
                lines.push_back(DetailText { .label = index == 0 ? std::string_view { detail.label } : ""sv,
                                             .text = std::move(wrapped[index]),
                                             .role = detail.role,
                                             .indent = index == 0 ? 0 : ContinuationIndent,
                                             .link = detail.link });
        }
        return lines;
    }

    /// @return A state box's line: @p lead, @p context cut so that what ⏎ does still shows after it, then the
    ///         row's other actions (actionHints()), as many whole ones as fit.
    [[nodiscard]] std::string boxLine(std::string lead, std::string_view context, Row const& row, int columns)
    {
        auto parts = std::vector<std::string> { std::move(lead) };
        auto hints = actionHints(&row);
        if (!context.empty())
        {
            auto const separators = 2 * columnsOf(HintSeparator);
            auto const primary = hints.empty() ? 0 : columnsOf(hints.front());
            parts.push_back(
                cut(context, std::max(1, columns - columnsOf(parts.front()) - primary - separators)));
        }
        std::ranges::move(hints, std::back_inserter(parts));
        auto const views =
            parts | std::views::transform([](std::string const& part) { return std::string_view { part }; })
            | std::ranges::to<std::vector>();
        return joinHints(views, columns);
    }

    /// @return The draft as the message box draws it: its lines, each wrapped to @p columns.
    [[nodiscard]] std::vector<std::string> draftLines(std::string_view draft, int columns)
    {
        auto lines = std::vector<std::string> {};
        for (auto const piece: std::views::split(draft, '\n'))
        {
            auto wrapped = wrap(std::string_view { piece.begin(), piece.end() }, columns, columns);
            if (wrapped.empty())
                wrapped.emplace_back();
            std::ranges::move(wrapped, std::back_inserter(lines));
        }
        if (lines.empty())
            lines.emplace_back();
        return lines;
    }

    /// @return The run @p row stands for; nullptr when it stands for none.
    [[nodiscard]] Run const* runOf(Engine const& engine, Row const* row) noexcept
    {
        if (row == nullptr || row->run.empty())
            return nullptr;
        auto const runs = engine.runs();
        auto const found = std::ranges::find(runs, row->run, &Run::id);
        return found != runs.end() ? &*found : nullptr;
    }

    /// Draws @p text over a box's top border, from its right end: a run's state.
    void putOnBorderRight(Canvas& canvas, std::string_view text, core::tui::Style const& style)
    {
        auto const shown = cut(text, std::max(0, (canvas.width() / 2) - 4));
        if (shown.empty())
            return;
        canvas.putString(0, canvas.width() - 3 - columnsOf(shown), std::format(" {} ", shown), style);
    }

    /// @return @p parts joined with ` · `, the empty ones left out.
    [[nodiscard]] std::string joinedParts(std::span<std::string const> parts)
    {
        auto text = std::string {};
        for (auto const& part: parts)
            if (!part.empty())
                text += text.empty() ? part : std::format("{}{}", HintSeparator, part);
        return text;
    }
} // namespace

Furniture furnitureGivingWay(FocusRows const& rows) noexcept
{
    // The border, and the strip, the header's line and latest while they stand.
    auto content = rows.height - 2 - 1 - 1 - rows.bottomRows;
    if (rows.tab != FocusTab::Transcript)
        content -= LatestRows;
    auto given = Furniture::None;
    for (auto const& [item, itemRows]: GivingWayOrder)
    {
        if (content >= MinFocusContentRows)
            break;
        // `latest` gives way only while the Board tab is open; the Transcript tab has none to give.
        if (item == Furniture::Latest && rows.tab != FocusTab::Board)
            continue;
        given = given | item;
        content += item == Furniture::TopBar ? rows.topBarRows : itemRows;
    }
    return given;
}

std::pair<std::size_t, std::size_t> FocusPanels::extentOf(FocusTab tab) const noexcept
{
    switch (tab)
    {
        case FocusTab::Board: return { board.lines, board.rows };
        case FocusTab::Transcript: return { transcript.lines, transcript.rows };
        case FocusTab::Details: break;
    }
    return { details.lines, details.rows };
}

BottomBox bottomBoxOf(Engine const& engine, Row const* row, Place place) noexcept
{
    if (row == nullptr)
        return BottomBox::None;
    if (row->kind == RowKind::Finished)
        return BottomBox::Ended;
    // An asking agent's menu takes keys, not messages: the asking box takes the message box's place.
    if (row->rowState == RowState::Asking)
        return BottomBox::Asking;
    if (!hasMessageBox(&engine, row))
        return BottomBox::None;
    // A done run's box names what follows; writing to it follows it up, in the message box.
    if (row->attention == FleetGroup::ToReview && place == Place::Fleet)
        return BottomBox::Done;
    return BottomBox::Message;
}

std::vector<std::vector<QuickReply const*>> chipLinesOf(std::span<QuickReply const> replies, int columns)
{
    auto lines = std::vector<std::vector<QuickReply const*>> { {} };
    auto used = 0;
    for (auto const& reply: replies)
    {
        auto const width = chipColumnsOf(reply);
        if (!lines.back().empty() && used + ChipGap + width > columns)
        {
            lines.emplace_back();
            used = 0;
        }
        used += (lines.back().empty() ? 0 : ChipGap) + width;
        lines.back().push_back(&reply);
    }
    return lines;
}

int bottomRowsOf(BottomBox box, Row const* row, std::string_view draft, int columns, int height)
{
    switch (box)
    {
        case BottomBox::None: return 0;
        case BottomBox::Done:
        case BottomBox::Ended: return OneLineBoxRows;
        // Its border, the request, and the chips.
        case BottomBox::Asking:
            return 3
                   + static_cast<int>(chipLinesOf(row != nullptr ? std::span { row->replies }
                                                                 : std::span<QuickReply const> {},
                                                  std::max(1, columns - ChipInset))
                                          .size());
        case BottomBox::Message: break;
    }
    // One line while empty; it grows with its draft, up to a third of the Focus.
    auto const lines = static_cast<int>(draftLines(draft, std::max(1, columns - 1)).size());
    return 2 + std::clamp(lines, 1, std::max(1, (height / 3) - 2));
}

Focus::Focus(Engine const& engine,
             Interaction const& interaction,
             FocusFrame const& frame,
             std::string const& tab,
             core::IClock const& clock,
             FocusPanels& panels,
             HitMap& hits):
    _engine { engine },
    _interaction { interaction },
    _frame { frame },
    _tab { tab },
    _panels { panels },
    _hits { hits },
    _board { engine, frame.subject, clock, panels.board },
    _transcript { engine, frame.subject, tab, panels.transcript }
{
}

HitRecorder Focus::recorder() const
{
    return HitRecorder { _hits, { .x = screenBounds().x, .y = screenBounds().y } };
}

void Focus::render(Canvas& canvas)
{
    // The wheel scrolls the tab under the pointer.
    recorder().add(canvas.area(), Hit { .kind = HitKind::FocusPane });
    auto const border = _frame.place == Place::Focus ? Role::Focus : Role::Border;
    auto const* const row = _frame.row;
    auto const isRunOrWorktree = row != nullptr && (!row->run.empty() || row->kind == RowKind::Orphan);
    if (!isRunOrWorktree)
    {
        drawFrame(canvas, {}, border);
        auto inside = insideFrame(canvas);
        auto const width = inside.width() - 2;
        auto lines = std::vector<std::pair<std::string, Role>> {};
        if (_frame.content == FleetContent::Unconfigured)
            lines.emplace_back("Nothing selected.", Role::Dim);
        else if (_frame.content == FleetContent::Empty)
            for (auto const line: Primer)
                lines.emplace_back(line, Role::Text);
        else if (row != nullptr && row->kind == RowKind::NewAgent)
            lines.emplace_back("⏎ or n starts an agent on a pull request, an issue, or anything you type.",
                               Role::Text);
        else if (row != nullptr && row->kind == RowKind::Heading)
            lines.emplace_back(std::format("{} · {} · enter or z unfolds it", row->text, row->status),
                               Role::Dim);
        auto y = 0;
        for (auto const& [text, role]: lines)
            for (auto const& piece: wrap(text, width, width))
                if (y < inside.height())
                    inside.putString(y++, 1, piece, styleOf(canvas.theme(), role));
        return;
    }
    renderRun(canvas, runOf(_engine, row));
}

void Focus::renderRun(Canvas& canvas, Run const* run)
{
    auto const& theme = canvas.theme();
    auto const& row = *_frame.row;
    auto const border = _frame.place == Place::Focus ? Role::Focus : Role::Border;
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - 2;

    auto const bottomRows = bottomRowsOf(_frame.box, &row, draftOf(), width, canvas.height());
    auto const given =
        _frame.givenWay
        | furnitureGivingWay(FocusRows {
            .height = canvas.height(), .topBarRows = 0, .bottomRows = bottomRows, .tab = _frame.tab });
    auto const tabLabel = std::ranges::find(TabLabels, _frame.tab, &TabLabel::tab);

    // The header: the run, its task and model on the border, its state on the right.
    auto title =
        run != nullptr ? std::format("{} · {}", row.text, run->model) : std::string { "leftover worktree" };
    if (holds(given, Furniture::TabStrip))
        title = std::format("{} · {}", title, tabLabel->word);
    drawFrame(canvas, title, border);
    putOnBorderRight(canvas,
                     run != nullptr ? std::format("{} {}", row.mark, row.status) : row.status,
                     styleOf(theme, run != nullptr ? row.statusRole : Role::Dim));

    auto y = 0;
    if (!holds(given, Furniture::HeaderDetail))
    {
        auto const parts = run != nullptr ? std::array { row.title, run->branch, run->permissions }
                                          : std::array { row.text, std::string {}, std::string {} };
        inside.putString(y++, 1, cut(joinedParts(parts), width), styleOf(theme, Role::Dim));
    }
    auto liveChip = std::optional<int> {};
    if (!holds(given, Furniture::TabStrip))
    {
        auto x = 1;
        for (auto const& label: TabLabels)
        {
            if (!std::ranges::contains(row.offers, label.command))
                continue;
            auto const active = label.tab == _frame.tab;
            auto style = styleOf(theme, active ? Role::Accent : Role::Dim);
            style.bold = active;
            style.underline = active;
            auto key = style;
            key.underline = true;
            recorder().shifted(1, 1).add(
                Rect { .x = x, .y = y, .width = columnsOf(label.label), .height = 1 },
                Hit { .kind = HitKind::Command, .value = std::to_underlying(label.command) });
            x += inside.putString(y, x, label.label.substr(0, 1), key);
            x += inside.putString(y, x, label.label.substr(1), style);
            if (_frame.tabNews[std::to_underlying(label.tab)] == News::Unseen)
                x += inside.putString(y, x, " •", styleOf(theme, Role::Accent));
            x += 2;
        }
        liveChip = y++;
    }
    auto const latestRows =
        run != nullptr && _frame.tab != FocusTab::Transcript && !holds(given, Furniture::Latest) ? LatestRows
                                                                                                 : 0;
    auto const bodyRows = std::max(0, inside.height() - y - latestRows - bottomRows);
    auto body = inside.subcanvas(Rect { .x = 0, .y = y, .width = inside.width(), .height = bodyRows });
    switch (_frame.tab)
    {
        case FocusTab::Board: _board.render(body); break;
        case FocusTab::Transcript: _transcript.render(body); break;
        case FocusTab::Details: renderDetails(body); break;
    }
    // While the transcript does not follow the agent, a chip says so; `end` returns.
    if (_frame.tab == FocusTab::Transcript && _frame.subject.follow == Follow::Away)
    {
        constexpr auto Live = " ↓ live "sv;
        auto const chipRow = liveChip.value_or(y + bodyRows - 1);
        if (chipRow >= 0 && chipRow < inside.height())
            inside.putString(
                chipRow, inside.width() - 1 - columnsOf(Live), Live, styleOf(theme, Role::Accent));
    }
    if (latestRows > 0)
    {
        auto latest = inside.subcanvas(
            Rect { .x = 0, .y = y + bodyRows, .width = inside.width(), .height = latestRows });
        renderLatest(latest, *run);
    }
    if (bottomRows > 0)
    {
        auto box = inside.subcanvas(Rect { .x = 1,
                                           .y = inside.height() - bottomRows,
                                           .width = std::max(0, inside.width() - 2),
                                           .height = bottomRows });
        renderBottom(box, recorder().shifted(2, 1 + inside.height() - bottomRows));
    }
}

void Focus::renderAsking(Canvas& canvas, HitRecorder const& hits)
{
    auto const& theme = canvas.theme();
    auto const& row = *_frame.row;
    auto const width = canvas.width() - 4;
    // Answered: the chip it was answered with fills, and the box dims until the agent stops asking.
    auto const& answered = _interaction.answered;
    auto const answeredKey =
        answered && answered->run == row.run ? std::optional { answered->key } : std::nullopt;
    auto const tone = answeredKey ? Role::Dim : Role::Warning;
    drawFrame(canvas, std::format("? {} asks", row.name), tone);
    auto inside = insideFrame(canvas);
    inside.putString(0, 1, cut(row.context, width), styleOf(theme, answeredKey ? Role::Dim : Role::Text));
    if (row.replies.empty())
    {
        inside.putString(1, 1, cut("answer it in its tab · r opens it", width), styleOf(theme, Role::Dim));
        return;
    }
    auto y = 1;
    for (auto const& line: chipLinesOf(row.replies, std::max(1, canvas.width() - ChipInset)))
    {
        auto x = 1;
        for (auto const* const reply: line)
        {
            auto key = styleOf(theme, tone);
            key.bold = true;
            auto label = styleOf(theme, answeredKey ? Role::Dim : Role::Text);
            if (answeredKey == reply->key)
            {
                key = styleOf(theme, Role::Accent);
                key.inverse = true;
                label = key;
            }
            // A click on a chip answers with it, as its digit does.
            hits.shifted(1, 1).add(Rect { .x = x,
                                          .y = y,
                                          .width = std::min(chipColumnsOf(*reply), std::max(0, width - x)),
                                          .height = 1 },
                                   Hit { .kind = HitKind::Command,
                                         .value = std::to_underlying(KeyCommand::Reply1)
                                                  + static_cast<std::size_t>(reply->key - '1') });
            x += inside.putString(y, x, std::string(1, reply->key), key);
            x += inside.putString(y, x, " ", label);
            x += inside.putString(y, x, cut(reply->label, std::max(0, width - x)), label) + ChipGap;
        }
        ++y;
    }
}

std::string_view Focus::draftOf() const noexcept
{
    auto const* const row = _frame.row;
    if (row == nullptr)
        return {};
    auto const draft = _interaction.drafts.find(row->run);
    return draft != _interaction.drafts.end() ? std::string_view { draft->second } : std::string_view {};
}

void Focus::renderDetails(Canvas& canvas)
{
    auto const& theme = canvas.theme();
    auto const width = canvas.width() - 2;
    auto const lines = detailLinesOf(_frame.row->details, width - LabelColumns);
    auto const rows = static_cast<std::size_t>(std::max(0, canvas.height()));
    _panels.details = DetailsPanelState { .lines = lines.size(), .rows = rows };
    auto const top = std::min(_frame.subject.scroll, lines.size() > rows ? lines.size() - rows : 0);
    auto y = 0;
    for (auto const& line: lines | std::views::drop(top) | std::views::take(rows))
    {
        canvas.putString(y, 1, cut(line.label, LabelColumns - 1), styleOf(theme, Role::Dim));
        auto const column = 1 + LabelColumns + line.indent;
        auto const columnSpan = canvas.putString(y, column, line.text, styleOf(theme, line.role));
        if (!line.link.empty() && columnSpan > 0)
            canvas.addHyperlink(y, column, columnSpan, line.link);
        ++y;
    }
}

void Focus::renderLatest(Canvas& canvas, Run const& run)
{
    auto const& theme = canvas.theme();
    auto const width = canvas.width() - 2;
    constexpr auto Heading = "── latest "sv;
    auto rule = std::string { Heading };
    for ([[maybe_unused]] auto const _:
         std::views::iota(columnsOf(Heading), std::max(columnsOf(Heading), width)))
        rule += "─";
    canvas.putString(0, 1, rule, styleOf(theme, Role::Border));

    auto lines = std::vector<TranscriptLine> {};
    auto const* const view = _engine.transcript(_tab, run.id);
    auto const* const page = view != nullptr && view->content ? &*view->content : nullptr;
    if (page != nullptr)
        for (auto const& entry: page->entries
                                    | std::views::drop(page->entries.size() > LatestEntries
                                                           ? page->entries.size() - LatestEntries
                                                           : 0))
            lines.push_back(latestLineOf(entry, width));
    // A message sent here shows until the transcript does.
    if (auto const& sent = _interaction.sent; sent && sent->run == run.id)
    {
        auto const shown =
            page != nullptr && std::ranges::any_of(page->entries, [&](TranscriptEntry const& entry) {
                return entry.kind == TranscriptEntryKind::User && entry.text == sent->text;
            });
        if (!shown)
            lines.push_back(TranscriptLine {
                .text = cut(std::format("you · sent {} · {}", sent->clock, sent->text), width),
                .role = Role::Accent });
    }
    if (lines.empty())
        lines.push_back(TranscriptLine { .text = view != nullptr && view->state == FetchState::Loading
                                                     ? "reading the transcript…"
                                                     : "nothing yet",
                                         .role = Role::Dim });
    auto y = 1;
    for (auto const& line:
         lines | std::views::drop(lines.size() > LatestEntries ? lines.size() - LatestEntries : 0))
        if (y < canvas.height())
            putLinked(canvas, y++, 1, line.text, styleOf(theme, line.role));
}

void Focus::renderBottom(Canvas& canvas, HitRecorder const& hits)
{
    auto const& theme = canvas.theme();
    auto const& row = *_frame.row;
    auto const width = canvas.width() - 4;
    switch (_frame.box)
    {
        case BottomBox::None: return;
        case BottomBox::Message: {
            // A click in the message box gives it the keys.
            hits.add(canvas.area(), Hit { .kind = HitKind::MessageBox });
            auto const draft = draftOf();
            if (_frame.place == Place::Focus)
            {
                drawFrame(canvas, std::format("to {}", row.name), Role::Focus);
                auto inside = insideFrame(canvas);
                auto lines = draftLines(draft, std::max(1, width - 1));
                lines.back() += Caret;
                // The end of a long draft, where the caret is, stays in view.
                auto const rows = static_cast<std::size_t>(std::max(0, inside.height()));
                auto y = 0;
                for (auto const& line:
                     lines | std::views::drop(lines.size() > rows ? lines.size() - rows : 0))
                    inside.putString(y++, 1, line, styleOf(theme, Role::Text));
                return;
            }
            drawFrame(canvas, {}, Role::Border);
            auto inside = insideFrame(canvas);
            auto const text = draft.empty() ? std::format("⏎ to write to {}", row.name)
                                            : std::format("draft: {} · ⏎ to go on", draft);
            inside.putString(0, 1, cut(text, width), styleOf(theme, Role::Dim));
            return;
        }
        case BottomBox::Asking: renderAsking(canvas, hits); return;
        case BottomBox::Done: {
            drawFrame(canvas, {}, Role::Passing);
            insideFrame(canvas).putString(
                0, 1, boxLine("✓ reported done", {}, row, width), styleOf(theme, Role::Passing));
            return;
        }
        case BottomBox::Ended: {
            drawFrame(canvas, {}, Role::Border);
            insideFrame(canvas).putString(
                0,
                1,
                boxLine(std::format("{} {}", row.mark, row.status), row.context, row, width),
                styleOf(theme, row.statusRole));
            return;
        }
    }
}

} // namespace orchestrator::view
