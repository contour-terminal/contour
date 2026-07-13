// SPDX-License-Identifier: Apache-2.0
#include <contour/CommandCatalog.h>

#include <format>
#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

namespace contour
{

namespace
{
    /// Builds the Command that runs @p action, taking its identity, title and description from the
    /// action itself. Every source funnels through here, so a command's presentation does not depend
    /// on which source happened to contribute it.
    ///
    /// Assembled from the catalog row and the arguments DIRECTLY rather than by calling commandId() +
    /// commandTitle(): each of those would independently re-walk the action's arguments and re-scan the
    /// catalog, so going through them would do all that work twice for every command built.
    [[nodiscard]] Command makeCommand(actions::Action action)
    {
        auto const& entry = actions::catalogEntry(action);
        auto const arguments = commandArguments(action);

        return Command {
            .id = arguments.id.empty() ? std::string(entry.name)
                                       : std::format("{}:{}", entry.name, arguments.id),
            .title = splitCamelCase(entry.name) + arguments.title,
            .description = std::string(entry.documentation),
            .action = std::move(action),
        };
    }

    /// Appends every action bound by @p bindings to @p target.
    ///
    /// Templated over the binding's input type so the key/char/mouse halves share this rather than
    /// restating it three times.
    template <typename Input>
    void appendBoundActions(std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& bindings,
                            std::vector<Command>& target)
    {
        for (auto const& binding: bindings)
            for (auto const& action: binding.binding)
                target.push_back(makeCommand(action));
    }
} // namespace

std::vector<Command> collectCommands(std::span<CommandSource const* const> sources)
{
    auto result = std::vector<Command> {};
    auto seen = std::unordered_set<std::string> {};

    for (auto const* source: sources)
    {
        if (source == nullptr)
            continue;

        for (auto& command: source->commands())
        {
            // First source wins, so a source given precedence can override a later one's version of
            // the same command (see collectCommands()'s contract).
            if (!seen.insert(command.id).second)
                continue;
            result.push_back(std::move(command));
        }
    }

    return result;
}

std::vector<Command> ActionCommandSource::commands() const
{
    auto result = std::vector<Command> {};
    result.reserve(actions::actionCatalog().size());

    for (auto const& entry: actions::actionCatalog())
    {
        // A parameterized action's prototype names no profile / no tab / no characters, so running it
        // would do nothing (or something arbitrary). Its concrete instances arrive via the other
        // sources, which know what argument to give it.
        if (actions::isParameterized(entry.prototype))
            continue;
        result.push_back(makeCommand(entry.prototype));
    }

    return result;
}

std::vector<Command> BoundCommandSource::commands() const
{
    auto result = std::vector<Command> {};

    appendBoundActions(_mappings.keyMappings, result);
    appendBoundActions(_mappings.charMappings, result);
    appendBoundActions(_mappings.mouseMappings, result);

    return result;
}

std::vector<Command> ProfileCommandSource::commands() const
{
    auto result = std::vector<Command> {};

    for (auto const& name: _config.profiles.value() | std::views::keys)
        result.push_back(makeCommand(actions::ChangeProfile { .name = name }));

    return result;
}

std::vector<Command> LayoutCommandSource::commands() const
{
    auto result = std::vector<Command> {};

    for (auto const& name: _config.layouts.value() | std::views::keys)
        result.push_back(makeCommand(actions::LaunchLayout { .name = name }));

    return result;
}

std::vector<Command> TabCommandSource::commands() const
{
    auto const titles = _tabs.tabTitles();
    auto result = std::vector<Command> {};
    result.reserve(titles.size());

    for (auto const index: std::views::iota(std::size_t { 0 }, titles.size()))
    {
        // SwitchToTab positions are 1-based (see its documentation), unlike the 0-based tab index.
        auto const position = static_cast<int>(index) + 1;
        auto command = makeCommand(actions::SwitchToTab { .position = position });

        // Override the derived "Switch To Tab 3" with the tab's own title, so the user picks the tab
        // they can SEE rather than counting positions along the strip. A tab with no title keeps the
        // derived form rather than rendering an empty quote.
        if (!titles[index].empty())
            command.title = std::format("Switch To Tab {}: {}", position, titles[index]);

        result.push_back(std::move(command));
    }

    return result;
}

} // namespace contour
