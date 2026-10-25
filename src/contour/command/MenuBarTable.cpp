// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenuTable.hpp>
#include <contour/command/MenuBarTable.hpp>
#include <contour/command/TitleBarContextMenu.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <span>

namespace contour::command
{

namespace
{
    using Table = detail::MenuTable<MenuBarState>;
    using Row = Table::Row;

    // {{{ Predicates
    bool hasWindow(MenuBarState const& state) noexcept
    {
        return state.window == WindowPresence::Present;
    }

    bool hasSelection(MenuBarState const& state) noexcept
    {
        return state.selection == SelectionPresence::Present;
    }

    bool canSpeak(MenuBarState const& state) noexcept
    {
        return state.speech == SpeechAvailability::Available;
    }

    bool isInputProtected(MenuBarState const& state) noexcept
    {
        return state.inputProtection == InputProtection::On;
    }

    bool hasMultipleTabs(MenuBarState const& state) noexcept
    {
        return state.tabCount > 1;
    }

    bool hasTabsToRight(MenuBarState const& state) noexcept
    {
        return state.activeTab < state.tabCount - 1;
    }

    bool hasMultiplePanes(MenuBarState const& state) noexcept
    {
        return state.paneCount > 1;
    }
    // }}}

    /// A row that needs a window to act on.
    [[nodiscard]] Row inWindow(Row const& row)
    {
        return row.enabledWhen(hasWindow).inScope(ActionScope::Window);
    }

    /// A row running @p action under @p title, acting on the application: it still runs with no window
    /// focused, as after ⌘M, where a row of the default ActionScope::Window does nothing.
    [[nodiscard]] Row appCommand(actions::Action const& action, std::string_view title)
    {
        return Table::command(action, title).inScope(ActionScope::Application);
    }

    /// A row running @p action under its catalogue title, needing a window.
    [[nodiscard]] Row windowCommand(actions::Action const& action, std::string_view title = {})
    {
        return inWindow(Table::command(action, title));
    }

    /// A submenu's builder: the template @p Template, resolved against the state.
    template <std::span<Row const> (*Template)()>
    [[nodiscard]] std::vector<ContextMenuEntry> rowsOf(MenuBarState const& state)
    {
        auto entries = Table::buildRows(Template(), state);
        detail::dropRedundantSeparators(entries);
        return entries;
    }

    // {{{ Dynamic rows
    [[nodiscard]] std::vector<ContextMenuEntry> layoutRows(MenuBarState const& state)
    {
        return std::ranges::to<std::vector>(state.layoutNames | std::views::transform([](auto const& name) {
                                                return detail::commandEntry(
                                                    name, actions::LaunchLayout { .name = name });
                                            }));
    }

    /// One row per tab, the active one check-marked.
    [[nodiscard]] std::vector<ContextMenuEntry> tabListRows(MenuBarState const& state)
    {
        return std::ranges::to<std::vector>(std::views::iota(0, static_cast<int>(state.tabTitles.size()))
                                            | std::views::transform([&](int index) {
                                                  return detail::commandEntry(
                                                      state.tabTitles[static_cast<std::size_t>(index)],
                                                      actions::SwitchToTab { .position = index + 1 },
                                                      index == state.activeTab ? RowCheck::Checked
                                                                               : RowCheck::Unchecked);
                                              }));
    }

    [[nodiscard]] std::vector<ContextMenuEntry> tabBarRows(MenuBarState const& state)
    {
        return buildTabBarMenu({ .visibility = state.tabBarVisibility, .position = state.tabBarPosition });
    }
    // }}}

    // {{{ Contour
    [[nodiscard]] std::span<Row const> applicationTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            appCommand(ShowAbout {}, "About Contour").withRole(MenuItemRole::About),
            Table::separator(),
            appCommand(OpenConfiguration {}, "Settings…").withRole(MenuItemRole::Preferences),
            appCommand(OpenConfiguration { .target = ConfigurationTarget::Editor },
                       "Open Configuration File"),
            appCommand(ReloadConfig {}, "Reload Configuration"),
            appCommand(ResetConfig {}, "Reset Configuration"),
            Table::separator(),
            appCommand(Quit {}, "Quit Contour").withRole(MenuItemRole::Quit),
        };
        return rows;
    }
    // }}}

    // {{{ Shell
    [[nodiscard]] std::span<Row const> lastCommandTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(CopyCommandLine {}, "Copy Last Command"),
            Table::command(CopyLastCommandPrompt {}, "Copy Last Prompt"),
            Table::command(CopyLastCommandOutput {}, "Copy Last Output"),
            Table::command(CopyLastCommandBlock {}, "Copy Last Prompt and Output"),
            Table::command(CopyPreviousMarkRange {}, "Copy Previous Mark Range"),
            Table::command(SelectCommandBlock {}, "Select Last Output"),
            Table::command(OpenCommandOutput {}, "Open Last Output in Pager"),
            Table::command(RerunCommand {}, "Rerun Last Command"),
            Table::separator(),
            Table::command(ClearToPrompt {}, "Clear to Prompt"),
        };
        return rows;
    }

    [[nodiscard]] std::span<Row const> shellTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            appCommand(NewTerminal {}, "New Window"),
            Table::command(CreateNewTab {}, "New Tab"),
            Table::submenu("New Tab with Profile", Table::newTabProfileRows),
            Table::attachToHostSubmenu(),
            Table::submenu("Launch Layout", layoutRows),
            Table::command(SaveLayout {}, "Save Layout…"),
            Table::separator(),
            windowCommand(SplitVertical {}, "Split Vertically"),
            windowCommand(SplitHorizontal {}, "Split Horizontally"),
            Table::separator(),
            inWindow(Table::changeProfileSubmenu()),
            windowCommand(ToggleInputProtection {}, "Read-Only").checkedWhen(isInputProtected),
            Table::separator(),
            Table::submenu("Commands", rowsOf<lastCommandTemplate>),
            Table::separator(),
            Table::command(SaveScreenshot {}, "Save Screenshot"),
            Table::command(CopyScreenshot {}, "Copy Screenshot"),
            Table::command(ScreenshotVT {}, "Save VT Screenshot"),
            Table::command(OpenFileManager {}, "Open Current Folder"),
            Table::separator(),
            Table::command(SoftReset {}, "Soft Reset"),
            Table::command(ClearHistoryAndReset {}, "Hard Reset"),
            Table::separator(),
            windowCommand(ClosePane {}, "Close Pane"),
            windowCommand(CloseTab {}, "Close Tab"),
            windowCommand(CloseAllTabs {}, "Close Window"),
        };
        return rows;
    }
    // }}}

    // {{{ Edit
    [[nodiscard]] std::span<Row const> speechTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(SpeakSelection {}, "Start Speaking").enabledWhen(hasSelection),
            Table::command(StopSpeaking {}, "Stop Speaking"),
        };
        return rows;
    }

    [[nodiscard]] std::span<Row const> editTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(CopySelection {}, "Copy").enabledWhen(hasSelection),
            Table::command(PasteClipboard {}, "Paste"),
            Table::command(PasteSelection {}, "Paste Selection"),
            Table::command(SelectAll {}, "Select All"),
            Table::command(CancelSelection {}, "Clear Selection").enabledWhen(hasSelection),
            Table::separator(),
            Table::command(SearchReverse {}, "Find…"),
            Table::command(FocusNextSearchMatch {}, "Find Next"),
            Table::command(FocusPreviousSearchMatch {}, "Find Previous"),
            Table::command(NoSearchHighlight {}, "Clear Find Highlight"),
            Table::separator(),
            Table::command(OpenSelection {}, "Open Selection").enabledWhen(hasSelection),
            // The default binding's exact action, so its chord shows.
            Table::command(HintMode { .patterns = "url", .hintAction = vtbackend::HintAction::Open },
                           "Open Link…"),
            Table::command(ViNormalMode {}, "Vi Mode"),
            Table::separator(),
            Table::submenu("Speech", rowsOf<speechTemplate>).shownWhen(canSpeak),
        };
        return rows;
    }
    // }}}

    // {{{ View
    [[nodiscard]] std::span<Row const> scrollTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(ScrollUp {}),
            Table::command(ScrollDown {}),
            Table::command(ScrollOneUp {}),
            Table::command(ScrollOneDown {}),
            Table::command(ScrollPageUp {}),
            Table::command(ScrollPageDown {}),
            Table::command(ScrollToTop {}),
            Table::command(ScrollToBottom {}),
            Table::separator(),
            Table::command(ScrollMarkUp {}, "Previous Prompt"),
            Table::command(ScrollMarkDown {}, "Next Prompt"),
        };
        return rows;
    }

    [[nodiscard]] std::span<Row const> foldsTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(ToggleFold {}),       Table::command(ToggleLastFold {}),
            Table::command(CollapseLastFold {}), Table::command(CollapseAllFolds {}),
            Table::command(ExpandAllFolds {}),
        };
        return rows;
    }

    [[nodiscard]] std::span<Row const> keyboardTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(ToggleAllKeyMaps {}),
            Table::command(ToggleInputMethodHandling {}),
        };
        return rows;
    }

    [[nodiscard]] std::span<Row const> viewTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(OpenCommandPalette {}, "Command Palette…"),
            Table::separator(),
            Table::command(IncreaseFontSize {}, "Bigger"),
            Table::command(DecreaseFontSize {}, "Smaller"),
            Table::command(ResetFontSize {}, "Actual Size"),
            Table::command(IncreaseOpacity {}, "More Opaque"),
            Table::command(DecreaseOpacity {}, "Less Opaque"),
            Table::separator(),
            inWindow(Table::submenu("Tab Bar", tabBarRows)),
            Table::command(ToggleStatusLine {}),
            Table::command(ToggleTitleBar {}),
            Table::command(ToggleFullscreen {}),
            Table::separator(),
            Table::submenu("Scroll", rowsOf<scrollTemplate>),
            Table::submenu("Folds", rowsOf<foldsTemplate>),
            Table::submenu("Keyboard", rowsOf<keyboardTemplate>),
        };
        return rows;
    }
    // }}}

    // {{{ Window
    [[nodiscard]] std::span<Row const> paneTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(FocusPaneLeft {}, "Focus Left"),
            Table::command(FocusPaneRight {}, "Focus Right"),
            Table::command(FocusPaneUp {}, "Focus Up"),
            Table::command(FocusPaneDown {}, "Focus Down"),
            Table::separator(),
            Table::command(SwapPaneLeft {}, "Swap Left"),
            Table::command(SwapPaneRight {}, "Swap Right"),
            Table::command(SwapPaneUp {}, "Swap Up"),
            Table::command(SwapPaneDown {}, "Swap Down"),
            Table::separator(),
            Table::command(MovePaneLeft {}, "Move Left"),
            Table::command(MovePaneRight {}, "Move Right"),
            Table::command(MovePaneUp {}, "Move Up"),
            Table::command(MovePaneDown {}, "Move Down"),
            Table::separator(),
            Table::command(ResizePane { .direction = Direction::Left }, "Grow Left"),
            Table::command(ResizePane { .direction = Direction::Right }, "Grow Right"),
            Table::command(ResizePane { .direction = Direction::Up }, "Grow Up"),
            Table::command(ResizePane { .direction = Direction::Down }, "Grow Down"),
            Table::separator(),
            Table::command(ToggleSplitOrientation {}),
            Table::command(TogglePaneZoom {}, "Zoom Pane"),
        };
        return rows;
    }

    [[nodiscard]] std::span<Row const> windowTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            windowCommand(MinimizeWindow {}, "Minimize").withRole(MenuItemRole::Minimize),
            windowCommand(ZoomWindow {}, "Zoom"),
            Table::separator(),
            windowCommand(SwitchToTabLeft {}, "Previous Tab"),
            windowCommand(SwitchToTabRight {}, "Next Tab"),
            windowCommand(SwitchToPreviousTab {}, "Last Used Tab"),
            Table::separator(),
            windowCommand(MoveTabToLeft {}, "Move Tab Left"),
            windowCommand(MoveTabToRight {}, "Move Tab Right"),
            windowCommand(MoveTabToNewWindow {}, "Move Tab to New Window").enabledWhen(hasMultipleTabs),
            windowCommand(SetTabTitle {}, "Rename Tab…"),
            windowCommand(SetTabColor {}, "Tab Color…"),
            windowCommand(ResetTabColor {}, "Reset Tab Color"),
            windowCommand(CloseOtherTabs {}, "Close Other Tabs").enabledWhen(hasMultipleTabs),
            windowCommand(CloseTabsToRight {}, "Close Tabs to the Right").enabledWhen(hasTabsToRight),
            Table::separator(),
            Table::submenu("Pane", rowsOf<paneTemplate>).enabledWhen(hasMultiplePanes),
            Table::separator(),
            Table::rows(tabListRows),
        };
        return rows;
    }
    // }}}

    // {{{ Help
    [[nodiscard]] std::span<Row const> debugTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(CreateDebugDump {}), Table::separator(),
            Table::command(TraceEnter {}),      Table::command(TraceLeave {}),
            Table::command(TraceStep {}),       Table::command(TraceBreakAtEmptyQueue {}),
        };
        return rows;
    }

    [[nodiscard]] std::span<Row const> helpTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            appCommand(FollowHyperlink { .uri = "https://contour-terminal.org/" }, "Contour Documentation"),
            appCommand(FollowHyperlink { .uri = "https://github.com/contour-terminal/contour/issues" },
                       "Report an Issue"),
            Table::separator(),
            Table::submenu("Debug", rowsOf<debugTemplate>),
        };
        return rows;
    }
    // }}}

    /// The menu bar itself: one submenu per MenuBarMenu, in that order.
    [[nodiscard]] std::span<Row const> menuBarTemplate()
    {
        static auto const rows = std::array {
            Table::submenu("Contour", rowsOf<applicationTemplate>), // MenuBarMenu::Application
            Table::submenu("Shell", rowsOf<shellTemplate>),         // MenuBarMenu::Shell
            Table::submenu("Edit", rowsOf<editTemplate>),           // MenuBarMenu::Edit
            Table::submenu("View", rowsOf<viewTemplate>),           // MenuBarMenu::View
            Table::submenu("Window", rowsOf<windowTemplate>),       // MenuBarMenu::Window
            Table::submenu("Help", rowsOf<helpTemplate>),           // MenuBarMenu::Help
        };
        static_assert(std::tuple_size_v<decltype(rows)> == MenuBarMenuCount);
        return rows;
    }

} // namespace

MenuBarState menuBarStateOf(config::Config const& config)
{
    auto sortedKeys = [](auto const& map) {
        auto names = std::ranges::to<std::vector<std::string>>(map | std::views::keys);
        std::ranges::sort(names);
        return names;
    };

    return MenuBarState { .profileNames = sortedKeys(config.profiles.value()),
                          .hostNames = sortedKeys(config.hosts.value()),
                          .layoutNames = sortedKeys(config.layouts.value()) };
}

std::vector<ContextMenuEntry> buildMenuBar(MenuBarState const& state)
{
    return Table::buildRows(menuBarTemplate(), state);
}

} // namespace contour::command
