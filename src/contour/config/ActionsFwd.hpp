// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <initializer_list>
#include <variant>
#include <vector>

namespace contour::actions
{

// Every alternative of Action, declared and not defined: a header that only names an action, or holds a
// list of them, includes this file and never pays for instantiating the 129-way variant. The definitions
// are in Actions.hpp.
struct CancelSelection;
struct ChangeProfile;
struct ClearHistoryAndReset;
struct CopyPreviousMarkRange;
struct CopySelection;
struct CreateDebugDump;
struct CreateSelection;
struct DecreaseFontSize;
struct DecreaseOpacity;
struct FocusNextSearchMatch;
struct FocusPreviousSearchMatch;
struct FollowHyperlink;
struct HintMode;
struct IncreaseFontSize;
struct IncreaseOpacity;
struct NewTerminal;
struct NewWindowTo;
struct NoSearchHighlight;
struct OpenCommandPalette;
struct OpenConfiguration;
struct OpenFileManager;
struct OpenSelection;
struct PasteClipboard;
struct PasteSelection;
struct Quit;
struct ReloadConfig;
struct ResetConfig;
struct ResetFontSize;
struct ScreenshotVT;
struct SaveScreenshot;
struct CopyScreenshot;
struct CollapseAllFolds;
struct CollapseLastFold;
struct ExpandAllFolds;
struct ScrollDown;
struct ScrollMarkDown;
struct ScrollMarkUp;
struct ScrollOneDown;
struct ScrollOneUp;
struct ScrollPageDown;
struct ScrollPageUp;
struct ScrollToBottom;
struct ScrollToTop;
struct ToggleFold;
struct ToggleFoldAt;
struct ToggleLastFold;
struct FocusPreviousCommand;
struct FocusNextCommand;
struct SelectPreviousCommandOutput;
struct SelectNextCommandOutput;
struct OpenBlockMenu;
struct ClearCommandBlockFocus;
struct ScrollUp;
struct SearchReverse;
struct SearchSelection;
struct SendChars;
struct ToggleAllKeyMaps;
struct ToggleFullscreen;
struct ToggleInputMethodHandling;
struct ToggleInputProtection;
struct ToggleStatusLine;
struct ToggleTitleBar;
struct TraceBreakAtEmptyQueue;
struct TraceEnter;
struct TraceLeave;
struct TraceStep;
struct ViNormalMode;
struct WriteScreen;
struct CreateNewTab;
struct CloseTab;
struct MoveTabTo;
struct MoveTabToLeft;
struct MoveTabToRight;
struct SwitchToTab;
struct SwitchToPreviousTab;
struct SwitchToTabLeft;
struct SwitchToTabRight;
struct SetTabTitle;
struct SplitVertical;
struct SplitHorizontal;
struct ClosePane;
struct FocusPaneLeft;
struct FocusPaneRight;
struct FocusPaneUp;
struct FocusPaneDown;
struct SwapPaneLeft;
struct SwapPaneRight;
struct SwapPaneUp;
struct SwapPaneDown;
struct MovePaneLeft;
struct MovePaneRight;
struct MovePaneUp;
struct MovePaneDown;
struct ToggleSplitOrientation;
struct TogglePaneZoom;
struct ResizePane;
struct LaunchLayout;
struct SaveLayout;
struct AttachToHost;
struct OpenContextMenu;
struct SelectAll;
struct SoftReset;
struct CopyLastCommandPrompt;
struct CopyLastCommandOutput;
struct CopyLastCommandBlock;
struct CopyHyperlink;
struct SetTabColor;
struct ResetTabColor;
struct CloseAllTabs;
struct SetTabBarVisibility;
struct SetTabBarPosition;
struct SpeakSelection;
struct StopSpeaking;
struct SelectCommandBlock;
struct CopyCommandBlock;
struct CopyCommandLine;
struct RerunCommand;
struct ToggleCommandFold;
struct ClearToPrompt;
struct OpenCommandOutput;
struct CloseOtherTabs;
struct CloseTabsToRight;
struct MoveTabToNewWindow;
struct ShowAbout;
struct MinimizeWindow;
struct ZoomWindow;
struct FocusNextArea;
struct FocusPreviousArea;
struct FocusToolbar;

/// What a key, a character or a mouse button is bound to; the user's actions, one command's worth.
///
/// Defined over the declared-only alternatives, so naming it costs nothing. Only a file that includes
/// Actions.hpp can look inside it.
using Action = std::variant<CancelSelection,
                            ChangeProfile,
                            ClearHistoryAndReset,
                            CopyPreviousMarkRange,
                            CopySelection,
                            CreateDebugDump,
                            CreateSelection,
                            DecreaseFontSize,
                            DecreaseOpacity,
                            FocusNextSearchMatch,
                            FocusPreviousSearchMatch,
                            FollowHyperlink,
                            HintMode,
                            IncreaseFontSize,
                            IncreaseOpacity,
                            NewTerminal,
                            NewWindowTo,
                            NoSearchHighlight,
                            OpenCommandPalette,
                            OpenConfiguration,
                            OpenFileManager,
                            OpenSelection,
                            PasteClipboard,
                            PasteSelection,
                            Quit,
                            ReloadConfig,
                            ResetConfig,
                            ResetFontSize,
                            ScreenshotVT,
                            SaveScreenshot,
                            CopyScreenshot,
                            CollapseAllFolds,
                            CollapseLastFold,
                            ExpandAllFolds,
                            ScrollDown,
                            ScrollMarkDown,
                            ScrollMarkUp,
                            ScrollOneDown,
                            ScrollOneUp,
                            ScrollPageDown,
                            ScrollPageUp,
                            ScrollToBottom,
                            ScrollToTop,
                            ToggleFold,
                            ToggleFoldAt,
                            ToggleLastFold,
                            FocusPreviousCommand,
                            FocusNextCommand,
                            SelectPreviousCommandOutput,
                            SelectNextCommandOutput,
                            OpenBlockMenu,
                            ClearCommandBlockFocus,
                            ScrollUp,
                            SearchReverse,
                            SearchSelection,
                            SendChars,
                            ToggleAllKeyMaps,
                            ToggleFullscreen,
                            ToggleInputMethodHandling,
                            ToggleInputProtection,
                            ToggleStatusLine,
                            ToggleTitleBar,
                            TraceBreakAtEmptyQueue,
                            TraceEnter,
                            TraceLeave,
                            TraceStep,
                            ViNormalMode,
                            WriteScreen,
                            CreateNewTab,
                            CloseTab,
                            MoveTabTo,
                            MoveTabToLeft,
                            MoveTabToRight,
                            SwitchToTab,
                            SwitchToPreviousTab,
                            SwitchToTabLeft,
                            SwitchToTabRight,
                            SetTabTitle,
                            SplitVertical,
                            SplitHorizontal,
                            ClosePane,
                            FocusPaneLeft,
                            FocusPaneRight,
                            FocusPaneUp,
                            FocusPaneDown,
                            SwapPaneLeft,
                            SwapPaneRight,
                            SwapPaneUp,
                            SwapPaneDown,
                            MovePaneLeft,
                            MovePaneRight,
                            MovePaneUp,
                            MovePaneDown,
                            ToggleSplitOrientation,
                            TogglePaneZoom,
                            ResizePane,
                            LaunchLayout,
                            SaveLayout,
                            AttachToHost,
                            OpenContextMenu,
                            SelectAll,
                            SoftReset,
                            CopyLastCommandPrompt,
                            CopyLastCommandOutput,
                            CopyLastCommandBlock,
                            CopyHyperlink,
                            SetTabColor,
                            ResetTabColor,
                            CloseAllTabs,
                            SetTabBarVisibility,
                            SetTabBarPosition,
                            SpeakSelection,
                            StopSpeaking,
                            SelectCommandBlock,
                            CopyCommandBlock,
                            CopyCommandLine,
                            RerunCommand,
                            ToggleCommandFold,
                            ClearToPrompt,
                            OpenCommandOutput,
                            CloseOtherTabs,
                            CloseTabsToRight,
                            MoveTabToNewWindow,
                            ShowAbout,
                            MinimizeWindow,
                            ZoomWindow,
                            FocusNextArea,
                            FocusPreviousArea,
                            FocusToolbar>;

/// An ordered list of actions: what one binding runs, or what one menu row stands for.
///
/// This is a class, rather than an alias of std::vector<Action>, so that the vector's copy, move and
/// destruction over the 129-way variant are instantiated once, in Actions.cpp, instead of in every
/// translation unit that copies or destroys a binding. The members below are the vector-like surface the
/// callers use; the element access needs the complete Action, so it is the includer of Actions.hpp that
/// reads the elements.
class ActionList
{
  public:
    using value_type = Action;                                  // NOLINT(readability-identifier-naming)
    using iterator = std::vector<Action>::iterator;             // NOLINT(readability-identifier-naming)
    using const_iterator = std::vector<Action>::const_iterator; // NOLINT(readability-identifier-naming)

    ActionList();
    ActionList(std::initializer_list<Action> actions);
    ActionList(ActionList const& other);
    ActionList(ActionList&& other) noexcept;
    ActionList& operator=(ActionList const& other);
    ActionList& operator=(ActionList&& other) noexcept;
    ~ActionList();

    // Element access. Declared here, defined inline in Actions.hpp once Action is complete.
    [[nodiscard]] iterator begin() noexcept;
    [[nodiscard]] iterator end() noexcept;
    [[nodiscard]] const_iterator begin() const noexcept;
    [[nodiscard]] const_iterator end() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Action& operator[](std::size_t index);
    [[nodiscard]] Action const& operator[](std::size_t index) const;
    [[nodiscard]] Action& at(std::size_t index);
    [[nodiscard]] Action const& at(std::size_t index) const;
    [[nodiscard]] Action& front();
    [[nodiscard]] Action const& front() const;

    /// Appends @p action.
    void push_back(Action action); // NOLINT(readability-identifier-naming)

    /// Removes every action.
    void clear() noexcept; // NOLINT(readability-identifier-naming)

  private:
    std::vector<Action> _actions;
};

} // namespace contour::actions
