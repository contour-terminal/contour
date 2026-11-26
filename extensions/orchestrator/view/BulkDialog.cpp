// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/BulkDialog.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    /// The keys line.
    constexpr auto BulkKeys = "←/→ task · ⏎ start · esc cancel"sv;
    /// What the dialog says where it would start nothing.
    constexpr auto NothingTakes = "no item here takes that task"sv;
    /// Where each agent runs: the dialog chooses the task alone.
    constexpr auto WhereItRuns = "each on its task's worker and model"sv;
    /// The widest the dialog draws its lines.
    constexpr auto BulkWidth = 56;

    /// @return The dialog's items, as the engine lists them now; an item no longer listed is left out.
    [[nodiscard]] std::vector<WorkItem> listedItems(Engine const& engine, BulkDialogState const& dialog)
    {
        auto const all = engine.workItems();
        auto items = std::vector<WorkItem> {};
        for (auto const& key: dialog.items)
            if (auto const found =
                    std::ranges::find_if(all, [&](WorkItem const& item) { return workItemKey(item) == key; });
                found != all.end())
                items.push_back(*found);
        return items;
    }

    /// @return Whether @p task (empty: the suggested one) starts on @p item.
    [[nodiscard]] bool takes(Config const& config, WorkItem const& item, std::string_view task)
    {
        if (!item.agentRun.empty())
            return false;
        if (task.empty())
            return suggestTask(config, item).task != nullptr;
        auto const* const def = config.findTask(task);
        return def != nullptr && def->appliesTo == appliesToOf(item.item);
    }

    /// @return The chosen task's id; empty for the suggested one.
    [[nodiscard]] std::string chosenTask(Engine const& engine, BulkDialogState const& dialog)
    {
        auto const choices = bulkChoices(engine, dialog);
        return dialog.choice < choices.size() ? choices[dialog.choice] : std::string {};
    }

    /// @return What @p count items of @p items are called: merge requests, tickets, or items when mixed.
    [[nodiscard]] std::string_view nounOf(std::span<WorkItem const> items, std::size_t count) noexcept
    {
        auto const mrs =
            std::ranges::count_if(items, [](WorkItem const& item) { return item.changeRequest.has_value(); });
        auto const one = count == 1;
        if (mrs == std::ranges::ssize(items))
            return one ? "merge request"sv : "merge requests"sv;
        if (mrs == 0)
            return one ? "ticket"sv : "tickets"sv;
        return one ? "item"sv : "items"sv;
    }
} // namespace

std::optional<BulkDialogState> openBulkDialog(Engine const& engine,
                                              WorkGroup group,
                                              std::span<WorkItem const> shown)
{
    if (engine.config() == nullptr)
        return std::nullopt;
    auto items = shown | std::views::filter([](WorkItem const& item) { return item.agentRun.empty(); })
                 | std::views::transform(workItemKey) | std::ranges::to<std::vector>();
    if (items.empty())
        return std::nullopt;
    return BulkDialogState { .group = group, .items = std::move(items), .choice = 0 };
}

std::vector<std::string> bulkChoices(Engine const& engine, BulkDialogState const& dialog)
{
    auto choices = std::vector<std::string> { std::string {} };
    auto const* const config = engine.config();
    if (config == nullptr)
        return choices;
    auto const items = listedItems(engine, dialog);
    for (auto const& task: config->tasks())
        if (task.appliesTo != AppliesTo::Topic && std::ranges::any_of(items, [&](WorkItem const& item) {
                return takes(*config, item, task.id);
            }))
            choices.push_back(task.id);
    return choices;
}

std::vector<std::string> bulkItems(Engine const& engine, BulkDialogState const& dialog)
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return {};
    auto const task = chosenTask(engine, dialog);
    return listedItems(engine, dialog)
           | std::views::filter([&](WorkItem const& item) { return takes(*config, item, task); })
           | std::views::transform(workItemKey) | std::ranges::to<std::vector>();
}

std::size_t bulkCount(Engine const& engine, BulkDialogState const& dialog)
{
    return bulkItems(engine, dialog).size();
}

std::vector<std::string> bulkLines(Engine const& engine, BulkDialogState const& dialog)
{
    auto const count = bulkCount(engine, dialog);
    auto const task = chosenTask(engine, dialog);
    auto const* const config = engine.config();
    auto const* const def = config != nullptr && !task.empty() ? config->findTask(task) : nullptr;
    auto const label = def != nullptr ? std::string_view { def->label } : "the suggested task"sv;
    auto const items = listedItems(engine, dialog);
    auto lines = std::vector<std::string> {};
    if (count == 0)
        lines.emplace_back(NothingTakes);
    else
        lines.push_back(std::format("Start {} on {} {}", label, count, nounOf(items, count)));
    lines.emplace_back(WhereItRuns);
    lines.emplace_back(BulkKeys);
    return lines;
}

core::tui::Size BulkDialogView::preferredSize() const
{
    // Its lines, a blank before the keys, and the box with a cell of padding on either side.
    return { .width = BulkWidth + 4, .height = 3 + 1 + 2 };
}

void BulkDialogView::render(core::tui::Canvas& canvas)
{
    auto const& theme = canvas.theme();
    canvas.clear(styleOf(theme, Role::Text));
    HitRecorder { _hits, { .x = screenBounds().x, .y = screenBounds().y } }.add(
        canvas.area(), Hit { .kind = HitKind::Popup });
    if (!_dialog)
        return;
    auto const name = WorkGroupRows[std::to_underlying(_dialog->group)].name;
    drawFrame(canvas, std::format("Run a task on {}", name), Role::Focus);
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - 2;
    auto const lines = bulkLines(_engine, *_dialog);
    inside.putString(0, 1, cut(lines[0], width), styleOf(theme, Role::Text));
    inside.putString(1, 1, cut(lines[1], width), styleOf(theme, Role::Dim));
    inside.putString(inside.height() - 1, 1, cut(lines[2], width), styleOf(theme, Role::Dim));
}

} // namespace orchestrator::view
