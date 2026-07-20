// SPDX-License-Identifier: Apache-2.0
#include <contour/LayoutBuilder.h>

#include <vtbackend/Color.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>

namespace contour
{

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
                out << YAML::Key << "directory" << YAML::Value
                    << std::filesystem::path { *pane.directory }.generic_string();
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
