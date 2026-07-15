// SPDX-License-Identifier: Apache-2.0
#include <contour/LayoutBuilder.h>

#include <vtbackend/Color.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <format>
#include <numeric>
#include <ranges>

#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

namespace contour
{

config::LayoutPane const& leftmostLeaf(config::LayoutPane const& node)
{
    config::LayoutPane const* current = &node;
    while (!current->isLeaf())
        current = &current->children.front();
    return *current;
}

double ratioForFirst(std::span<config::LayoutPane const> children)
{
    if (children.empty())
        return 0.5;

    // A specified `ratio` is that child's fraction of the whole split; children without one share
    // the remaining space equally (so `ratio: 0.6` next to an unspecified sibling means 60/40).
    // MinimumShare keeps degenerate inputs (over-committed or non-positive fractions) from
    // collapsing a pane to zero; the final division re-normalizes whatever the shares add up to.
    auto constexpr MinimumShare = 0.01;
    auto const specifiedTotal =
        std::accumulate(children.begin(), children.end(), 0.0, [](double acc, auto const& child) {
            return acc + child.ratio.value_or(0.0);
        });
    auto const unspecifiedCount =
        std::ranges::count_if(children, [](auto const& child) { return !child.ratio.has_value(); });
    auto const defaultShare =
        unspecifiedCount > 0
            ? std::max((1.0 - specifiedTotal) / static_cast<double>(unspecifiedCount), MinimumShare)
            : MinimumShare;
    auto const share = [&](config::LayoutPane const& child) {
        return std::max(child.ratio.value_or(defaultShare), MinimumShare);
    };
    auto const total =
        std::accumulate(children.begin(), children.end(), 0.0, [&](double acc, auto const& child) {
            return acc + share(child);
        });
    return share(children.front()) / total;
}

double ratioForFirst(config::LayoutPane const& splitNode)
{
    return ratioForFirst(std::span<config::LayoutPane const> { splitNode.children });
}

namespace
{
    void realizePane(vtmux::SessionModel& model,
                     vtmux::TabId tab,
                     config::LayoutPane const& node,
                     PaneSeeder const& seed);

    /// Realizes @p children as the siblings of one split, splitting the model's active leaf once
    /// per child. Walks a span rather than materializing a "tail group" node at each level: the
    /// tail is the rest of the SAME children vector, so no subtree is ever copied.
    /// Precondition: the model's active leaf is a freshly created leaf seeded with
    /// leftmostLeaf(children.front()).
    void realizeSiblings(vtmux::SessionModel& model,
                         vtmux::TabId tab,
                         vtmux::SplitState orientation,
                         std::span<config::LayoutPane const> children,
                         PaneSeeder const& seed)
    {
        if (children.size() == 1)
        {
            // No sibling left to split against: the active leaf hosts this child's subtree.
            realizePane(model, tab, children.front(), seed);
            return;
        }

        // The current active leaf will host children[0]'s subtree; remember its id to return to it.
        auto const firstLeafId = model.findTab(tab)->activePane()->id();

        auto const tail = children.subspan(1);
        // Stage the backing session for the NEW pane = leftmost leaf of the tail group.
        seed(leftmostLeaf(tail.front()));
        model.splitActivePane(tab, orientation, ratioForFirst(children));

        // Active is now the tail's leftmost leaf: build the remaining siblings in place.
        realizeSiblings(model, tab, orientation, tail, seed);

        // Return to the first child's slot and build it.
        model.setActivePane(tab, firstLeafId);
        realizePane(model, tab, children.front(), seed);
    }

    // Precondition: model's active leaf is a freshly created leaf seeded with leftmostLeaf(node).
    void realizePane(vtmux::SessionModel& model,
                     vtmux::TabId tab,
                     config::LayoutPane const& node,
                     PaneSeeder const& seed)
    {
        if (node.isLeaf())
            return; // active leaf already carries this command

        realizeSiblings(model, tab, node.orientation, node.children, seed);
    }
} // namespace

vtmux::Tab* realizeLayoutTab(vtmux::SessionModel& model,
                             vtmux::WindowId window,
                             config::LayoutTab const& tab,
                             PaneSeeder const& seed)
{
    // Refuse before creating: seeding spawns a real backing session, so an unknown window must be
    // rejected BEFORE the first seed() runs — otherwise that session is orphaned when createTab
    // fails (mirrors createSessionInBackground's refuse-before-create guard).
    if (model.window(window) == nullptr)
        return nullptr;

    // Seed + create the tab's first pane (the root's leftmost leaf).
    seed(leftmostLeaf(tab.root));
    auto* modelTab = model.createTab(window);
    if (modelTab == nullptr)
        return nullptr;

    if (tab.title)
        model.setTabTitle(modelTab->id(), *tab.title);
    if (tab.color)
        model.setTabColor(modelTab->id(), vtmux::TabColorSource::User, *tab.color);

    realizePane(model, modelTab->id(), tab.root, seed);
    return modelTab;
}

namespace
{
    /// Escapes each `${` as `$${` so the loader's environment-variable expansion (see
    /// YAMLConfigReader::resolveVariables and crispy::replaceVariables' `$${` escape) reproduces
    /// the literal text instead of substituting it: a saved command like `s/${VERSION}/1.0/` must
    /// survive the save/load round trip verbatim. Only the fields the parser expands (command and
    /// arguments) need this; YAML quoting itself is the emitter's job.
    /// @param value The value about to be emitted.
    /// @return @p value with every variable marker escaped.
    [[nodiscard]] std::string escapeVariableMarkers(std::string const& value)
    {
        std::string out;
        out.reserve(value.size());
        auto position = std::string::size_type { 0 };
        while (true)
        {
            auto const marker = value.find("${", position);
            if (marker == std::string::npos)
                break;
            out.append(value, position, marker - position);
            out += "$${";
            position = marker + 2;
        }
        out += value.substr(position);
        return out;
    }

    void emitPane(YAML::Emitter& out, config::LayoutPane const& pane, bool emitRatio);

    /// Emits @p pane's keys into the mapping the caller has already opened. A tab's root pane and a
    /// split's child pane share this body: the parser reads a leaf's fields off the same node that
    /// carries `title`/`color`, so both live at one YAML level.
    /// @param out       The emitter, positioned inside an open mapping.
    /// @param pane      The pane (leaf or split) to emit.
    /// @param emitRatio Whether @p pane is a child of a split, and so carries a size within it. A
    ///                  tab's root pane has no governing parent split and never does.
    void emitPaneBody(YAML::Emitter& out, config::LayoutPane const& pane, bool emitRatio)
    {
        // Only an explicitly-sized pane pays the extra key, so an even split's YAML stays clean.
        if (emitRatio && pane.ratio)
            out << YAML::Key << "ratio" << YAML::Value << *pane.ratio;

        if (pane.isLeaf())
        {
            // An engaged-but-empty command means "the profile's default shell" — nothing to persist.
            if (pane.command && !pane.command->empty())
                // shellQuote so a program path containing spaces is not re-split by shellSplit() on reload.
                out << YAML::Key << "command" << YAML::Value
                    << escapeVariableMarkers(config::shellQuote(*pane.command));
            if (!pane.arguments.empty())
            {
                out << YAML::Key << "arguments" << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (auto const& argument: pane.arguments)
                    out << escapeVariableMarkers(argument);
                out << YAML::EndSeq;
            }
            if (pane.directory)
                // generic_string() (forward slashes) so a Windows path never carries a literal
                // backslash into the emitted YAML in the first place.
                out << YAML::Key << "directory" << YAML::Value << pane.directory->generic_string();
            if (pane.profile)
                out << YAML::Key << "profile" << YAML::Value << *pane.profile;
            return;
        }

        out << YAML::Key << "split" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "orientation" << YAML::Value
            << (pane.orientation == vtmux::SplitState::Horizontal ? "horizontal" : "vertical");
        out << YAML::Key << "panes" << YAML::Value << YAML::BeginSeq;
        for (auto const& child: pane.children)
            emitPane(out, child, /* emitRatio */ true);
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }

    /// Emits @p pane as one item of a `panes:` sequence (its own mapping).
    void emitPane(YAML::Emitter& out, config::LayoutPane const& pane, bool emitRatio)
    {
        out << YAML::BeginMap;
        emitPaneBody(out, pane, emitRatio);
        out << YAML::EndMap;
    }

    /// Emits @p tab as one item of a `tabs:` sequence: title/color/profile plus its root pane's own
    /// keys, all siblings in one mapping (matching how the parser reads them off the tab node).
    void emitTab(YAML::Emitter& out, config::LayoutTab const& tab)
    {
        out << YAML::BeginMap;
        if (tab.title)
            out << YAML::Key << "title" << YAML::Value << *tab.title;
        if (tab.color)
            out << YAML::Key << "color" << YAML::Value << vtbackend::formatColor(*tab.color);
        if (tab.profile)
            out << YAML::Key << "profile" << YAML::Value << *tab.profile;
        emitPaneBody(out, tab.root, /* emitRatio */ false);
        out << YAML::EndMap;
    }
} // namespace

config::LayoutPane serializePane(vtmux::Pane const& pane, LeafResolver const& resolve)
{
    config::LayoutPane out;
    if (pane.isLeaf())
    {
        auto const data = resolve(pane.session());
        out.command = data.command;
        out.arguments = data.arguments;
        if (data.directory)
            out.directory = std::filesystem::path { *data.directory };
        out.profile = data.profile;
        return out;
    }
    out.orientation = pane.splitState();
    out.children.push_back(serializePane(*pane.first(), resolve));
    out.children.push_back(serializePane(*pane.second(), resolve));
    // Preserve the split position as the first child's fraction (and its complement for the second).
    out.children.front().ratio = pane.ratio();
    out.children.back().ratio = 1.0 - pane.ratio();
    return out;
}

config::LayoutTab serializeTab(vtmux::Tab const& tab, LeafResolver const& resolve)
{
    config::LayoutTab out;
    if (tab.runtimeTitle())
        out.title = *tab.runtimeTitle();
    if (auto const color = tab.color(vtmux::TabColorSource::User))
        out.color = *color;
    out.root = serializePane(*tab.rootPane(), resolve);
    return out;
}

std::string emitLayoutsYaml(std::unordered_map<std::string, config::Layout> const& layouts)
{
    // Iterate layout names in SORTED order rather than the unordered_map's hash order, so
    // SaveLayout produces stable, diff-friendly output run to run regardless of insertion/hash
    // order. (Within a layout, `tabs` is already an ordered vector.)
    auto names = std::vector<std::string> {};
    names.reserve(layouts.size());
    for (auto const& name: layouts | std::views::keys)
        names.push_back(name);
    std::ranges::sort(names);

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "layouts" << YAML::Value << YAML::BeginMap;
    for (auto const& name: names)
    {
        out << YAML::Key << name << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "tabs" << YAML::Value << YAML::BeginSeq;
        for (auto const& tab: layouts.at(name).tabs)
            emitTab(out, tab);
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }
    out << YAML::EndMap;
    out << YAML::EndMap;

    return std::string { out.c_str() } + "\n";
}

} // namespace contour
