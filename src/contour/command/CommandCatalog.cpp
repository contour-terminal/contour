// SPDX-License-Identifier: Apache-2.0
#include <contour/command/CommandCatalog.hpp>
#include <contour/config/HostUrl.hpp>

#include <array>
#include <format>
#include <ranges>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace contour::command
{

namespace
{
    /// The category column (see actionCategories()), in the catalogue's own order.
    constexpr auto ActionCategories = std::array {
        ActionCategory { .name = "CancelSelection", .category = CommandCategory::Edit },
        ActionCategory { .name = "ChangeProfile", .category = CommandCategory::Profiles },
        ActionCategory { .name = "ClearHistoryAndReset", .category = CommandCategory::Shell },
        ActionCategory { .name = "CopyPreviousMarkRange", .category = CommandCategory::Blocks },
        ActionCategory { .name = "CopySelection", .category = CommandCategory::Edit },
        ActionCategory { .name = "CreateDebugDump", .category = CommandCategory::Shell },
        ActionCategory { .name = "CreateSelection", .category = CommandCategory::Edit },
        ActionCategory { .name = "DecreaseFontSize", .category = CommandCategory::View },
        ActionCategory { .name = "DecreaseOpacity", .category = CommandCategory::View },
        ActionCategory { .name = "FocusNextSearchMatch", .category = CommandCategory::Edit },
        ActionCategory { .name = "FocusPreviousSearchMatch", .category = CommandCategory::Edit },
        ActionCategory { .name = "FollowHyperlink", .category = CommandCategory::Edit },
        ActionCategory { .name = "HintMode", .category = CommandCategory::Edit },
        ActionCategory { .name = "IncreaseFontSize", .category = CommandCategory::View },
        ActionCategory { .name = "IncreaseOpacity", .category = CommandCategory::View },
        ActionCategory { .name = "NewTerminal", .category = CommandCategory::Window },
        ActionCategory { .name = "NoSearchHighlight", .category = CommandCategory::Edit },
        ActionCategory { .name = "OpenCommandPalette", .category = CommandCategory::Contour },
        ActionCategory { .name = "OpenConfiguration", .category = CommandCategory::Contour },
        ActionCategory { .name = "OpenFileManager", .category = CommandCategory::Shell },
        ActionCategory { .name = "OpenSelection", .category = CommandCategory::Edit },
        ActionCategory { .name = "PasteClipboard", .category = CommandCategory::Edit },
        ActionCategory { .name = "PasteSelection", .category = CommandCategory::Edit },
        ActionCategory { .name = "Quit", .category = CommandCategory::Contour },
        ActionCategory { .name = "ReloadConfig", .category = CommandCategory::Contour },
        ActionCategory { .name = "ResetConfig", .category = CommandCategory::Contour },
        ActionCategory { .name = "ResetFontSize", .category = CommandCategory::View },
        ActionCategory { .name = "ScreenshotVT", .category = CommandCategory::Shell },
        ActionCategory { .name = "SaveScreenshot", .category = CommandCategory::Shell },
        ActionCategory { .name = "CopyScreenshot", .category = CommandCategory::Shell },
        ActionCategory { .name = "CollapseAllFolds", .category = CommandCategory::Blocks },
        ActionCategory { .name = "CollapseLastFold", .category = CommandCategory::Blocks },
        ActionCategory { .name = "ExpandAllFolds", .category = CommandCategory::Blocks },
        ActionCategory { .name = "ScrollDown", .category = CommandCategory::View },
        ActionCategory { .name = "ScrollMarkDown", .category = CommandCategory::Blocks },
        ActionCategory { .name = "ScrollMarkUp", .category = CommandCategory::Blocks },
        ActionCategory { .name = "ScrollOneDown", .category = CommandCategory::View },
        ActionCategory { .name = "ScrollOneUp", .category = CommandCategory::View },
        ActionCategory { .name = "ScrollPageDown", .category = CommandCategory::View },
        ActionCategory { .name = "ScrollPageUp", .category = CommandCategory::View },
        ActionCategory { .name = "ScrollToBottom", .category = CommandCategory::View },
        ActionCategory { .name = "ScrollToTop", .category = CommandCategory::View },
        ActionCategory { .name = "ToggleFold", .category = CommandCategory::Blocks },
        ActionCategory { .name = "ToggleFoldAt", .category = CommandCategory::Blocks },
        ActionCategory { .name = "ToggleLastFold", .category = CommandCategory::Blocks },
        ActionCategory { .name = "ScrollUp", .category = CommandCategory::View },
        ActionCategory { .name = "SearchReverse", .category = CommandCategory::Edit },
        ActionCategory { .name = "SendChars", .category = CommandCategory::Shell },
        ActionCategory { .name = "ToggleAllKeyMaps", .category = CommandCategory::Shell },
        ActionCategory { .name = "ToggleFullscreen", .category = CommandCategory::Window },
        ActionCategory { .name = "ToggleInputMethodHandling", .category = CommandCategory::Shell },
        ActionCategory { .name = "ToggleInputProtection", .category = CommandCategory::Shell },
        ActionCategory { .name = "ToggleStatusLine", .category = CommandCategory::View },
        ActionCategory { .name = "ToggleTitleBar", .category = CommandCategory::Window },
        ActionCategory { .name = "TraceBreakAtEmptyQueue", .category = CommandCategory::Shell },
        ActionCategory { .name = "TraceEnter", .category = CommandCategory::Shell },
        ActionCategory { .name = "TraceLeave", .category = CommandCategory::Shell },
        ActionCategory { .name = "TraceStep", .category = CommandCategory::Shell },
        ActionCategory { .name = "ViNormalMode", .category = CommandCategory::Shell },
        ActionCategory { .name = "WriteScreen", .category = CommandCategory::Shell },
        ActionCategory { .name = "CreateNewTab", .category = CommandCategory::Tab },
        ActionCategory { .name = "CloseTab", .category = CommandCategory::Tab },
        ActionCategory { .name = "MoveTabTo", .category = CommandCategory::Tab },
        ActionCategory { .name = "MoveTabToLeft", .category = CommandCategory::Tab },
        ActionCategory { .name = "MoveTabToRight", .category = CommandCategory::Tab },
        ActionCategory { .name = "SwitchToTab", .category = CommandCategory::Tab },
        ActionCategory { .name = "SwitchToPreviousTab", .category = CommandCategory::Tab },
        ActionCategory { .name = "SwitchToTabLeft", .category = CommandCategory::Tab },
        ActionCategory { .name = "SwitchToTabRight", .category = CommandCategory::Tab },
        ActionCategory { .name = "SetTabTitle", .category = CommandCategory::Tab },
        ActionCategory { .name = "SplitVertical", .category = CommandCategory::Pane },
        ActionCategory { .name = "SplitHorizontal", .category = CommandCategory::Pane },
        ActionCategory { .name = "ClosePane", .category = CommandCategory::Pane },
        ActionCategory { .name = "FocusPaneLeft", .category = CommandCategory::Pane },
        ActionCategory { .name = "FocusPaneRight", .category = CommandCategory::Pane },
        ActionCategory { .name = "FocusPaneUp", .category = CommandCategory::Pane },
        ActionCategory { .name = "FocusPaneDown", .category = CommandCategory::Pane },
        ActionCategory { .name = "SwapPaneLeft", .category = CommandCategory::Pane },
        ActionCategory { .name = "SwapPaneRight", .category = CommandCategory::Pane },
        ActionCategory { .name = "SwapPaneUp", .category = CommandCategory::Pane },
        ActionCategory { .name = "SwapPaneDown", .category = CommandCategory::Pane },
        ActionCategory { .name = "MovePaneLeft", .category = CommandCategory::Pane },
        ActionCategory { .name = "MovePaneRight", .category = CommandCategory::Pane },
        ActionCategory { .name = "MovePaneUp", .category = CommandCategory::Pane },
        ActionCategory { .name = "MovePaneDown", .category = CommandCategory::Pane },
        ActionCategory { .name = "ToggleSplitOrientation", .category = CommandCategory::Pane },
        ActionCategory { .name = "TogglePaneZoom", .category = CommandCategory::Pane },
        ActionCategory { .name = "ResizePane", .category = CommandCategory::Pane },
        ActionCategory { .name = "LaunchLayout", .category = CommandCategory::Layouts },
        ActionCategory { .name = "SaveLayout", .category = CommandCategory::Layouts },
        ActionCategory { .name = "AttachToHost", .category = CommandCategory::Hosts },
        ActionCategory { .name = "OpenContextMenu", .category = CommandCategory::Shell },
        ActionCategory { .name = "SelectAll", .category = CommandCategory::Edit },
        ActionCategory { .name = "SoftReset", .category = CommandCategory::Shell },
        ActionCategory { .name = "CopyLastCommandPrompt", .category = CommandCategory::Blocks },
        ActionCategory { .name = "CopyLastCommandOutput", .category = CommandCategory::Blocks },
        ActionCategory { .name = "CopyLastCommandBlock", .category = CommandCategory::Blocks },
        ActionCategory { .name = "CopyHyperlink", .category = CommandCategory::Edit },
        ActionCategory { .name = "SetTabColor", .category = CommandCategory::Tab },
        ActionCategory { .name = "ResetTabColor", .category = CommandCategory::Tab },
        ActionCategory { .name = "CloseAllTabs", .category = CommandCategory::Window },
        ActionCategory { .name = "SetTabBarVisibility", .category = CommandCategory::Tab },
        ActionCategory { .name = "SetTabBarPosition", .category = CommandCategory::Tab },
        ActionCategory { .name = "SpeakSelection", .category = CommandCategory::Edit },
        ActionCategory { .name = "StopSpeaking", .category = CommandCategory::Edit },
        ActionCategory { .name = "SelectCommandBlock", .category = CommandCategory::Blocks },
        ActionCategory { .name = "CopyCommandBlock", .category = CommandCategory::Blocks },
        ActionCategory { .name = "CopyCommandLine", .category = CommandCategory::Blocks },
        ActionCategory { .name = "RerunCommand", .category = CommandCategory::Blocks },
        ActionCategory { .name = "ClearToPrompt", .category = CommandCategory::Blocks },
        ActionCategory { .name = "OpenCommandOutput", .category = CommandCategory::Blocks },
        ActionCategory { .name = "CloseOtherTabs", .category = CommandCategory::Tab },
        ActionCategory { .name = "CloseTabsToRight", .category = CommandCategory::Tab },
        ActionCategory { .name = "MoveTabToNewWindow", .category = CommandCategory::Tab },
        ActionCategory { .name = "ShowAbout", .category = CommandCategory::Contour },
        ActionCategory { .name = "MinimizeWindow", .category = CommandCategory::Window },
        ActionCategory { .name = "ZoomWindow", .category = CommandCategory::Window },
    };

    // A missing row would make commandCategory() read past the end; a misplaced one is the test's to catch.
    static_assert(ActionCategories.size() == std::variant_size_v<actions::Action>,
                  "every catalogue action needs exactly one ActionCategories row");

    /// The argument part of a command title's suffix: @p titleSuffix without one leading ": " or " ".
    [[nodiscard]] std::string argumentOf(std::string_view titleSuffix)
    {
        for (auto const prefix: { std::string_view { ": " }, std::string_view { " " } })
            if (titleSuffix.starts_with(prefix))
                return std::string(titleSuffix.substr(prefix.size()));
        return std::string(titleSuffix);
    }

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
        auto const category = commandCategory(action);

        return Command {
            .id = arguments.id.empty() ? std::string(entry.name)
                                       : std::format("{}:{}", entry.name, arguments.id),
            .title = splitCamelCase(entry.name) + arguments.title,
            .description = std::string(entry.documentation),
            .action = std::move(action),
            .category = category,
            .argument = argumentOf(arguments.title),
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

std::span<ActionCategory const> actionCategories() noexcept
{
    return ActionCategories;
}

CommandCategory commandCategory(actions::Action const& action) noexcept
{
    return ActionCategories[action.index()].category;
}

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

std::vector<Command> HostCommandSource::commands() const
{
    auto result = std::vector<Command> {};

    for (auto const& [name, entry]: _config.hosts.value())
    {
        auto command = makeCommand(actions::AttachToHost { .name = name });
        // The host's address says more about this row than the action's generic sentence does.
        command.description = config::toString(entry.url);
        result.push_back(std::move(command));
    }

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
        {
            command.title = std::format("Switch To Tab {}: {}", position, titles[index]);
            command.argument = std::format("{}: {}", position, titles[index]);
        }

        result.push_back(std::move(command));
    }

    return result;
}

} // namespace contour::command
