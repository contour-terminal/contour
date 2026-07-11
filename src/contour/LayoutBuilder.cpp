// SPDX-License-Identifier: Apache-2.0
#include <contour/LayoutBuilder.h>

#include <format>
#include <numeric>

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

double ratioForFirst(config::LayoutPane const& splitNode)
{
    if (splitNode.isLeaf() || splitNode.children.empty())
        return 0.5;
    double const total = std::accumulate(
        splitNode.children.begin(), splitNode.children.end(), 0.0, [](double acc, auto const& child) {
            return acc + (child.ratio > 0.0 ? child.ratio : 1.0);
        });
    double const first = splitNode.children.front().ratio > 0.0 ? splitNode.children.front().ratio : 1.0;
    if (total <= 0.0)
        return 0.5;
    return first / total;
}

config::LayoutPane tailGroup(config::LayoutPane const& splitNode)
{
    // children[1..] as a group. With exactly one remaining child, return it directly.
    if (splitNode.children.size() == 2)
        return splitNode.children[1];
    config::LayoutPane group;
    group.orientation = splitNode.orientation;
    group.children.assign(splitNode.children.begin() + 1, splitNode.children.end());
    return group;
}

namespace
{
    // Precondition: model's active leaf is a freshly created leaf seeded with leftmostLeaf(node).
    void realizePane(vtmux::SessionModel& model,
                     vtmux::TabId tab,
                     config::LayoutPane const& node,
                     PaneSeeder const& seed)
    {
        if (node.isLeaf())
            return; // active leaf already carries this command

        // The current active leaf will host children[0]'s subtree; remember its id to return to it.
        auto* active = model.findTab(tab)->activePane();
        auto const firstLeafId = active->id();

        auto const tail = tailGroup(node);
        // Stage the backing session for the NEW pane = leftmost leaf of the tail group.
        seed(leftmostLeaf(tail));
        model.splitActivePane(tab, node.orientation, ratioForFirst(node));

        // Active is now the tail's leftmost leaf: build the tail subtree in place.
        realizePane(model, tab, tail, seed);

        // Return to the first child's slot and build it.
        model.setActivePane(tab, firstLeafId);
        realizePane(model, tab, node.children.front(), seed);
    }
} // namespace

vtmux::Tab* realizeLayoutTab(vtmux::SessionModel& model,
                             vtmux::WindowId window,
                             config::LayoutTab const& tab,
                             PaneSeeder const& seed)
{
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
    std::string indentCols(int columns)
    {
        return std::string(static_cast<size_t>(columns), ' ');
    }

    std::string quoted(std::string const& value)
    {
        return "\"" + value + "\"";
    }

    // The parser reconstructs a color via `RGBColor{std::string}`, which only accepts the exact
    // 7-character "#RRGGBB" form (see vtbackend::RGBColor::operator=(std::string const&)). That is
    // NOT what `std::format("{}", color)` yields (it delegates to `to_string(RGBColor)`, which
    // wraps the hex in literal single quotes: `'#RRGGBB'`), so format the hex ourselves here.
    std::string colorHex(vtbackend::RGBColor color)
    {
        return std::format("#{:02X}{:02X}{:02X}", color.red, color.green, color.blue);
    }

    void emitChildPane(std::string& out, config::LayoutPane const& pane, int dashCol);

    // Emits the body of `pane` (its command/arguments/directory/profile, or its `split:` block)
    // as the tail of a list item whose dash sits at column `dashCol`. `wrote` is true if a
    // sibling key (title/color/profile, or an earlier pane field) has already claimed the dash
    // line; in that case every subsequent key — including this pane's first — starts its own
    // line at `dashCol + 2`. If `wrote` is still false, this pane's first key is placed right
    // after the dash itself.
    void emitPaneBody(std::string& out, config::LayoutPane const& pane, int dashCol, bool& wrote)
    {
        int const contentCol = dashCol + 2;
        auto emitKV = [&](std::string const& line) {
            if (!wrote)
            {
                out += " " + line + "\n";
                wrote = true;
            }
            else
                out += indentCols(contentCol) + line + "\n";
        };

        if (pane.isLeaf())
        {
            if (pane.command)
                emitKV("command: " + quoted(*pane.command));
            if (!pane.arguments.empty())
            {
                std::string args = "arguments: [";
                for (size_t i = 0; i < pane.arguments.size(); ++i)
                    args += (i ? ", " : "") + quoted(pane.arguments[i]);
                args += "]";
                emitKV(args);
            }
            if (pane.directory)
                emitKV("directory: " + quoted(pane.directory->string()));
            if (pane.profile)
                emitKV("profile: " + quoted(*pane.profile));
            return;
        }

        // Split node: `orientation:`/`panes:` always land two columns past wherever the
        // `split:` key itself started — whether that was on the dash line (bare pane) or on
        // its own line (tab already wrote title/color/profile), sibling keys always align to
        // `dashCol + 2`, so the body is always at `dashCol + 4`.
        emitKV("split:");
        int const bodyCol = dashCol + 4;
        out += indentCols(bodyCol) + "orientation: "
               + (pane.orientation == vtmux::SplitState::Horizontal ? "horizontal" : "vertical") + "\n";
        out += indentCols(bodyCol) + "panes:\n";
        int const childDashCol = bodyCol + 2;
        for (auto const& child: pane.children)
            emitChildPane(out, child, childDashCol);
    }

    void emitChildPane(std::string& out, config::LayoutPane const& pane, int dashCol)
    {
        out += indentCols(dashCol) + "-";
        bool wrote = false;
        emitPaneBody(out, pane, dashCol, wrote);
    }

    // Emits one tab as a `tabs:` list item. The dash's tail carries title/color/profile (only
    // those that are set), immediately followed by the root pane's own keys as further siblings
    // in the same mapping (a leaf tab's `command`/`arguments`/`directory`/`profile` live at the
    // same YAML level as `title`/`color`, matching how the parser reads them off the tab node).
    void emitTab(std::string& out, config::LayoutTab const& tab, int dashCol)
    {
        out += indentCols(dashCol) + "-";
        bool wrote = false;
        int const contentCol = dashCol + 2;
        auto emitKV = [&](std::string const& line) {
            if (!wrote)
            {
                out += " " + line + "\n";
                wrote = true;
            }
            else
                out += indentCols(contentCol) + line + "\n";
        };

        if (tab.title)
            emitKV("title: " + quoted(*tab.title));
        if (tab.color)
            emitKV("color: " + quoted(colorHex(*tab.color)));
        if (tab.profile)
            emitKV("profile: " + quoted(*tab.profile));

        emitPaneBody(out, tab.root, dashCol, wrote);
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
        return out;
    }
    out.orientation = pane.splitState();
    out.children.push_back(serializePane(*pane.first(), resolve));
    out.children.push_back(serializePane(*pane.second(), resolve));
    // Preserve the split position as the first child's weight (and its complement for the second).
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
    std::string out = "layouts:\n";
    for (auto const& [name, layout]: layouts)
    {
        out += indentCols(2) + name + ":\n";
        out += indentCols(4) + "tabs:\n";
        for (auto const& tab: layout.tabs)
            emitTab(out, tab, 6);
    }
    return out;
}

} // namespace contour
