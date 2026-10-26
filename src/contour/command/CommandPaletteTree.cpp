// SPDX-License-Identifier: Apache-2.0
#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandPaletteTree.hpp>
#include <contour/command/FuzzyFilter.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <utility>

namespace contour::command
{

namespace
{
    /// Between the footer's two key hints: "↩ Run · ⎋ Close".
    constexpr auto HintSeparator = std::string_view { " · " };

    /// A row of the listing before it is placed: the entry plus what it is ranked and grouped by.
    struct Candidate
    {
        PaletteEntry entry {};                            ///< The row as it will be listed.
        CommandCategory category = CommandCategory::Pane; ///< The group it lands in.
        int score = 0;                                    ///< The fuzzy score; 0 when unfiltered.
        std::size_t recency = 0; ///< Position in the recent list; its size when absent (sorts last).
    };

    /// Ranks @p a before @p b: higher score, then more recent, then title.
    [[nodiscard]] bool rankedBefore(Candidate const& a, Candidate const& b) noexcept
    {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.recency != b.recency)
            return a.recency < b.recency;
        return a.entry.title < b.entry.title;
    }

    /// Orders by title alone, the browsing order of a group.
    [[nodiscard]] bool titledBefore(Candidate const& a, Candidate const& b) noexcept
    {
        return a.entry.title < b.entry.title;
    }

    /// The position of @p id in @p recent, or `recent.size()` when absent.
    [[nodiscard]] std::size_t recencyOf(std::span<std::string const> recent, std::string_view id) noexcept
    {
        return static_cast<std::size_t>(std::ranges::distance(recent.begin(), std::ranges::find(recent, id)));
    }

    /// The index into @p tree's commands of the command whose id is @p id.
    [[nodiscard]] std::optional<std::size_t> commandIndexOf(PaletteTree const& tree,
                                                            std::string_view id) noexcept
    {
        auto const found = std::ranges::find(tree.commands, id, &Command::id);
        if (found == tree.commands.end())
            return std::nullopt;
        return static_cast<std::size_t>(std::ranges::distance(tree.commands.begin(), found));
    }

    /// The parent whose children include command @p commandIndex, or null for a plain command.
    [[nodiscard]] PaletteParent const* parentOf(PaletteTree const& tree, std::size_t commandIndex) noexcept
    {
        auto const found = std::ranges::find_if(tree.parents, [commandIndex](auto const& parent) {
            return std::ranges::contains(parent.children, commandIndex);
        });
        return found == tree.parents.end() ? nullptr : &*found;
    }

    /// The leading part of a child's qualified title: "Change Profile · ".
    [[nodiscard]] std::string qualifierOf(PaletteParent const& parent)
    {
        return std::format("{}{}", parent.title, DrillInSeparator);
    }

    /// Sets the group header on the first row of @p rows, appended to @p out.
    void appendGroup(std::vector<Candidate>& rows,
                     std::string_view groupTitle,
                     std::vector<PaletteEntry>& out)
    {
        for (auto& row: rows)
            out.push_back(std::move(row.entry));
        out[out.size() - rows.size()].groupTitle = groupTitle;
    }

    /// The top level, unfiltered: Recently used, then every non-empty group by title.
    [[nodiscard]] std::vector<PaletteEntry> listTop(PaletteTree const& tree,
                                                    std::span<std::string const> recent)
    {
        auto result = std::vector<PaletteEntry> {};

        auto recentRows = std::vector<Candidate> {};
        for (auto const& id: recent)
        {
            // An id that no longer resolves (a deleted profile, a closed tab) quietly drops out.
            auto const index = commandIndexOf(tree, id);
            if (!index)
                continue;
            auto const& command = tree.commands[*index];
            auto const* parent = parentOf(tree, *index);
            recentRows.push_back(Candidate {
                .entry =
                    PaletteEntry { .kind = PaletteEntryKind::Command,
                                   .index = *index,
                                   .title = parent ? qualifierOf(*parent) + command.argument : command.title,
                                   .titleMatches = {},
                                   .groupTitle = {} },
                .category = command.category,
                .score = 0,
                .recency = 0,
            });
        }
        if (!recentRows.empty())
            appendGroup(recentRows, RecentGroupTitle, result);

        auto buckets = std::array<std::vector<Candidate>, CommandCategoryCount> {};
        for (auto const index: tree.plain)
        {
            auto const& command = tree.commands[index];
            buckets[static_cast<std::size_t>(command.category)].push_back(Candidate {
                .entry = PaletteEntry { .kind = PaletteEntryKind::Command,
                                        .index = index,
                                        .title = command.title,
                                        .titleMatches = {},
                                        .groupTitle = {} },
                .category = command.category,
                .score = 0,
                .recency = 0,
            });
        }
        for (auto const index: std::views::iota(std::size_t { 0 }, tree.parents.size()))
        {
            auto const& parent = tree.parents[index];
            buckets[static_cast<std::size_t>(parent.category)].push_back(Candidate {
                .entry = PaletteEntry { .kind = PaletteEntryKind::DrillIn,
                                        .index = index,
                                        .title = parent.title,
                                        .titleMatches = {},
                                        .groupTitle = {} },
                .category = parent.category,
                .score = 0,
                .recency = 0,
            });
        }

        for (auto const& group: PaletteGroups)
        {
            auto& bucket = buckets[static_cast<std::size_t>(group.category)];
            if (bucket.empty())
                continue;
            std::ranges::stable_sort(bucket, titledBefore);
            appendGroup(bucket, group.title, result);
        }

        return result;
    }

    /// The top level, filtered: every match, grouped, groups by their best match.
    [[nodiscard]] std::vector<PaletteEntry> listTopFiltered(PaletteTree const& tree,
                                                            PaletteQuery const& query)
    {
        auto buckets = std::array<std::vector<Candidate>, CommandCategoryCount> {};
        auto const add = [&](Candidate candidate) {
            buckets[static_cast<std::size_t>(candidate.category)].push_back(std::move(candidate));
        };

        for (auto const index: tree.plain)
        {
            auto const& command = tree.commands[index];
            // The title is what the user reads and types at; the id is the fallback ("splitv" finds
            // "Split Vertical"), and highlights nothing, there being no id on screen.
            auto score = std::optional<int> {};
            auto matches = std::vector<int> {};
            if (auto match = fuzzyMatch(query.filter, command.title))
            {
                score = match->score;
                matches = std::move(match->positions);
            }
            else
                score = fuzzyScore(query.filter, command.id);
            if (!score)
                continue;
            add(Candidate { .entry = PaletteEntry { .kind = PaletteEntryKind::Command,
                                                    .index = index,
                                                    .title = command.title,
                                                    .titleMatches = std::move(matches),
                                                    .groupTitle = {} },
                            .category = command.category,
                            .score = *score,
                            .recency = recencyOf(query.recent, command.id) });
        }

        for (auto const parentIndex: std::views::iota(std::size_t { 0 }, tree.parents.size()))
        {
            auto const& parent = tree.parents[parentIndex];
            auto const qualifier = qualifierOf(parent);
            auto parentRecency = query.recent.size();

            // A child matches on its argument alone: the parent's words would otherwise surface every
            // child at once ("change" listing all two hundred profiles).
            for (auto const index: parent.children)
            {
                auto const& command = tree.commands[index];
                auto const recency = recencyOf(query.recent, command.id);
                parentRecency = std::min(parentRecency, recency);
                auto match = fuzzyMatch(query.filter, command.argument);
                if (!match)
                    continue;
                for (auto& position: match->positions)
                    position += static_cast<int>(qualifier.size());
                add(Candidate { .entry = PaletteEntry { .kind = PaletteEntryKind::Command,
                                                        .index = index,
                                                        .title = qualifier + command.argument,
                                                        .titleMatches = std::move(match->positions),
                                                        .groupTitle = {} },
                                .category = command.category,
                                .score = match->score,
                                .recency = recency });
            }

            auto score = std::optional<int> {};
            auto matches = std::vector<int> {};
            if (auto match = fuzzyMatch(query.filter, parent.title))
            {
                score = match->score;
                matches = std::move(match->positions);
            }
            else
                score = fuzzyScore(query.filter, parent.name);
            if (!score)
                continue;
            add(Candidate { .entry = PaletteEntry { .kind = PaletteEntryKind::DrillIn,
                                                    .index = parentIndex,
                                                    .title = parent.title,
                                                    .titleMatches = std::move(matches),
                                                    .groupTitle = {} },
                            .category = parent.category,
                            .score = *score,
                            .recency = parentRecency });
        }

        // Each group ranked within; the groups ordered by their best row, ties by table order, so row 0
        // is always the best match.
        auto groups = std::vector<PaletteGroupRow> {};
        for (auto const& group: PaletteGroups)
        {
            auto& bucket = buckets[static_cast<std::size_t>(group.category)];
            if (bucket.empty())
                continue;
            std::ranges::stable_sort(bucket, rankedBefore);
            groups.push_back(group);
        }
        std::ranges::stable_sort(groups, [&](PaletteGroupRow const& a, PaletteGroupRow const& b) {
            return buckets[static_cast<std::size_t>(a.category)].front().score
                   > buckets[static_cast<std::size_t>(b.category)].front().score;
        });

        auto result = std::vector<PaletteEntry> {};
        for (auto const& group: groups)
            appendGroup(buckets[static_cast<std::size_t>(group.category)], group.title, result);
        return result;
    }

    /// Inside one parent's level: its children by argument, without headers.
    [[nodiscard]] std::vector<PaletteEntry> listLevel(PaletteTree const& tree, PaletteQuery const& query)
    {
        if (*query.level >= tree.parents.size())
            return {};

        auto rows = std::vector<Candidate> {};
        for (auto const index: tree.parents[*query.level].children)
        {
            auto const& command = tree.commands[index];
            auto match = fuzzyMatch(query.filter, command.argument);
            if (!match)
                continue;
            rows.push_back(Candidate { .entry = PaletteEntry { .kind = PaletteEntryKind::Command,
                                                               .index = index,
                                                               .title = command.argument,
                                                               .titleMatches = std::move(match->positions),
                                                               .groupTitle = {} },
                                       .category = command.category,
                                       .score = match->score,
                                       .recency = recencyOf(query.recent, command.id) });
        }

        // Unfiltered, every score is 0, so this is recency then argument: the recently used first.
        std::ranges::stable_sort(rows, rankedBefore);

        auto result = std::vector<PaletteEntry> {};
        result.reserve(rows.size());
        for (auto& row: rows)
            result.push_back(std::move(row.entry));
        return result;
    }
} // namespace

Command const* PaletteTree::find(std::string_view id) const noexcept
{
    auto const found = std::ranges::find(commands, id, &Command::id);
    return found == commands.end() ? nullptr : &*found;
}

PaletteTree buildPaletteTree(std::vector<Command> commands)
{
    auto tree = PaletteTree { .commands = std::move(commands), .plain = {}, .parents = {} };

    auto const& catalog = actions::actionCatalog();
    auto childrenByAction = std::vector<std::vector<std::size_t>>(catalog.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, tree.commands.size()))
    {
        auto const& action = tree.commands[index].action;
        if (actions::isParameterized(action))
            childrenByAction[action.index()].push_back(index);
        else
            tree.plain.push_back(index);
    }

    // Parents in catalogue order; each one's children by argument, the order its level lists them in
    // (the sources hand them over in hash-map order, which would make the level shuffle between runs).
    for (auto const actionIndex: std::views::iota(std::size_t { 0 }, catalog.size()))
    {
        auto& children = childrenByAction[actionIndex];
        if (children.empty())
            continue;
        std::ranges::stable_sort(children, [&](std::size_t a, std::size_t b) {
            return tree.commands[a].argument < tree.commands[b].argument;
        });
        auto const& entry = catalog[actionIndex];
        tree.parents.push_back(PaletteParent { .name = std::string(entry.name),
                                               .title = splitCamelCase(entry.name),
                                               .description = std::string(entry.documentation),
                                               .category = commandCategory(entry.prototype),
                                               .children = std::move(children) });
    }

    return tree;
}

std::vector<PaletteEntry> listPalette(PaletteTree const& tree, PaletteQuery const& query)
{
    if (query.level)
        return listLevel(tree, query);
    if (query.filter.empty())
        return listTop(tree, query.recent);
    return listTopFiltered(tree, query);
}

std::string paletteFooterHints(PaletteEntryKind kind, PaletteDepth depth, ShortcutStyle style)
{
    auto const row = std::ranges::find_if(PaletteHintRows, [&](PaletteHintRow const& hint) {
        return hint.kind == kind && hint.depth == depth;
    });
    if (row == PaletteHintRows.end())
        return {};

    auto const key = [style](vtbackend::Key input) {
        return shortcutText(ShortcutChord { .modifiers = {}, .input = input }, style);
    };
    return std::format("{} {}{}{} {}",
                       key(vtbackend::Key::Enter),
                       row->enterVerb,
                       HintSeparator,
                       key(vtbackend::Key::Escape),
                       row->escapeVerb);
}

} // namespace contour::command
