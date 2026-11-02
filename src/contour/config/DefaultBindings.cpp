// SPDX-License-Identifier: Apache-2.0

#include <contour/config/Actions.hpp>
#include <contour/config/DefaultBindings.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace contour::config
{

namespace
{
    using vtbackend::MatchModes;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using enum PlatformScope;

    // ---- The modes a row requires ------------------------------------------------------------------------

    /// No mode requirement.
    constexpr auto Any = MatchModes {};
    /// Select and Insert, as the built-in copy and paste rows have always required them.
    constexpr auto SelectAndInsert = MatchModes { MatchModes::Select | MatchModes::Insert, 0 };
    /// Insert mode.
    constexpr auto InsertOnly = MatchModes { MatchModes::Insert, 0 };
    /// A search in progress.
    constexpr auto SearchOnly = MatchModes { MatchModes::Search, 0 };
    /// The modes of a built-in row that acts on command blocks, which exist on the primary screen only.
    constexpr auto PrimaryScreenOnly = MatchModes { 0, MatchModes::AlternateScreen };
    /// The modes of a built-in row that acts on the focused command block, so that without one the key
    /// reaches the application.
    constexpr auto RequiresBlockFocus = MatchModes { MatchModes::BlockFocus, 0 };
    /// Escape in vi's select mode is CancelSelection's: the block's own Escape stands aside there, so that no
    /// key press can fire both.
    constexpr auto BlockFocusOutsideSelect = MatchModes { MatchModes::BlockFocus, MatchModes::Select };

    // ---- The chords a row can have -----------------------------------------------------------------------

    /// No modifiers.
    constexpr auto NoMods = ChordSpec {};
    constexpr auto Shift = ChordSpec { .extra = Modifiers { Modifier::Shift } };
    constexpr auto Alt = ChordSpec { .extra = Modifiers { Modifier::Alt } };
    constexpr auto Ctrl = ChordSpec { .extra = Modifiers { Modifier::Control } };
    constexpr auto Super = ChordSpec { .extra = Modifiers { Modifier::Super } };
    constexpr auto CtrlShift = ChordSpec { .extra = Modifiers { Modifier::Control, Modifier::Shift } };
    constexpr auto CtrlAlt = ChordSpec { .extra = Modifiers { Modifier::Control, Modifier::Alt } };
    constexpr auto AltShift = ChordSpec { .extra = Modifiers { Modifier::Alt, Modifier::Shift } };
    constexpr auto CtrlShiftAlt =
        ChordSpec { .extra = Modifiers { Modifier::Control, Modifier::Shift, Modifier::Alt } };
    constexpr auto SuperShift = ChordSpec { .extra = Modifiers { Modifier::Super, Modifier::Shift } };
    constexpr auto SuperAlt = ChordSpec { .extra = Modifiers { Modifier::Super, Modifier::Alt } };
    /// The host's Primary set: Command on macOS, Ctrl+Shift elsewhere.
    constexpr auto Primary = ChordSpec { .primary = PrimaryUse::Present };
    /// Primary plus Shift: Shift+Command on macOS, which is where macOS spells these chords, and still
    /// Ctrl+Shift elsewhere, where Primary already holds Shift.
    constexpr auto PrimaryShift =
        ChordSpec { .extra = Modifiers { Modifier::Shift }, .primary = PrimaryUse::Present };

    // ---- Row builders ------------------------------------------------------------------------------------

    /// Builds a row.
    template <typename Input>
    [[nodiscard]] DefaultBindingRow<Input> makeRow(PlatformScope scope,
                                                   RowReach reach,
                                                   ChordSpec chord,
                                                   Input input,
                                                   MatchModes modes,
                                                   ActionList binding,
                                                   DeclinedInput declined = DeclinedInput::PassThrough)
    {
        auto row = DefaultBindingRow<Input> { .scope = scope,
                                              .reach = reach,
                                              .modes = modes,
                                              .chord = chord,
                                              .input = input,
                                              .declined = declined };
        row.binding = std::move(binding);
        return row;
    }

    /// A row of the generated configuration only.
    template <typename Input>
    [[nodiscard]] DefaultBindingRow<Input> written(
        PlatformScope scope, ChordSpec chord, Input input, MatchModes modes, ActionList binding)
    {
        return makeRow(scope, RowReach::WrittenOnly, chord, input, modes, std::move(binding));
    }

    /// A row of the fallback tables only: a configuration that predates it still gets it.
    template <typename Input>
    [[nodiscard]] DefaultBindingRow<Input> fallback(PlatformScope scope,
                                                    ChordSpec chord,
                                                    Input input,
                                                    MatchModes modes,
                                                    ActionList binding,
                                                    DeclinedInput declined = DeclinedInput::PassThrough)
    {
        return makeRow(scope, RowReach::FallbackOnly, chord, input, modes, std::move(binding), declined);
    }

    /// A row of the generated configuration that is also a fallback.
    template <typename Input>
    [[nodiscard]] DefaultBindingRow<Input> both(
        PlatformScope scope, ChordSpec chord, Input input, MatchModes modes, ActionList binding)
    {
        return makeRow(scope, RowReach::WrittenAndFallback, chord, input, modes, std::move(binding));
    }

    /// @p row, restricted to the newest defaults edition.
    template <typename Input>
    [[nodiscard]] DefaultBindingRow<Input> native2026Only(DefaultBindingRow<Input> row)
    {
        row.edition = EditionScope::Native2026Only;
        return row;
    }

    /// @p row, restricted to the oldest defaults edition.
    template <typename Input>
    [[nodiscard]] DefaultBindingRow<Input> classicOnly(DefaultBindingRow<Input> row)
    {
        row.edition = EditionScope::ClassicOnly;
        return row;
    }

    /// @p row, taking part in matching only while @p predicate holds.
    template <typename Input>
    [[nodiscard]] DefaultBindingRow<Input> gatedBy(DefaultBindingRow<Input> row,
                                                   bool (*predicate)(Config const&) noexcept)
    {
        row.enabled = predicate;
        return row;
    }

    /// @p row, with the command-block click count @p count.
    template <typename Input>
    [[nodiscard]] DefaultBindingRow<Input> withClickCount(DefaultBindingRow<Input> row, uint8_t count)
    {
        row.clickCount = count;
        return row;
    }

    // ---- The tables --------------------------------------------------------------------------------------
    //
    // Match order is table order, restricted to the rows of the platform, edition and reach asked for. So a
    // row that must be found before another one, in either of the two tables it is in, is simply earlier.

    /// The key rows. The macOS-only rows follow the shared ones they replace on the desktop.
    [[nodiscard]] std::vector<DefaultBindingRow<vtbackend::Key>> const& keyRows()
    {
        using vtbackend::Key;
        static auto const rows = std::vector<DefaultBindingRow<Key>> {
            written(All, Alt, Key::Enter, Any, { actions::ToggleFullscreen {} }),
            written(All, NoMods, Key::Escape, SelectAndInsert, { actions::CancelSelection {} }),
            written(All, Shift, Key::DownArrow, Any, { actions::ScrollOneDown {} }),
            written(All, Shift, Key::End, Any, { actions::ScrollToBottom {} }),
            written(All, Shift, Key::Home, Any, { actions::ScrollToTop {} }),
            written(All, Shift, Key::PageDown, Any, { actions::ScrollPageDown {} }),
            written(All, Shift, Key::PageUp, Any, { actions::ScrollPageUp {} }),
            written(All, Shift, Key::UpArrow, Any, { actions::ScrollOneUp {} }),
            written(All, NoMods, Key::F3, SearchOnly, { actions::FocusNextSearchMatch {} }),
            written(All, Shift, Key::F3, SearchOnly, { actions::FocusPreviousSearchMatch {} }),
            written(All, Shift, Key::LeftArrow, Any, { actions::SwitchToTabLeft {} }),
            written(All, Shift, Key::RightArrow, Any, { actions::SwitchToTabRight {} }),
            both(All, Ctrl, Key::PageUp, Any, { actions::SwitchToTabLeft {} }),
            both(All, Ctrl, Key::PageDown, Any, { actions::SwitchToTabRight {} }),
            both(All, Ctrl, Key::Tab, Any, { actions::SwitchToTabRight {} }),
            both(All, CtrlShift, Key::Tab, Any, { actions::SwitchToTabLeft {} }),
            written(All, CtrlAlt, Key::LeftArrow, Any, { actions::SwapPaneLeft {} }),
            written(All, CtrlAlt, Key::RightArrow, Any, { actions::SwapPaneRight {} }),
            written(All, CtrlAlt, Key::UpArrow, Any, { actions::SwapPaneUp {} }),
            written(All, CtrlAlt, Key::DownArrow, Any, { actions::SwapPaneDown {} }),
            written(All,
                    CtrlShiftAlt,
                    Key::LeftArrow,
                    Any,
                    { actions::ResizePane { .direction = actions::Direction::Left } }),
            written(All,
                    CtrlShiftAlt,
                    Key::RightArrow,
                    Any,
                    { actions::ResizePane { .direction = actions::Direction::Right } }),
            written(All,
                    CtrlShiftAlt,
                    Key::UpArrow,
                    Any,
                    { actions::ResizePane { .direction = actions::Direction::Up } }),
            written(All,
                    CtrlShiftAlt,
                    Key::DownArrow,
                    Any,
                    { actions::ResizePane { .direction = actions::Direction::Down } }),
            // Beside the browsers' Ctrl+PageUp/PageDown tab switching above, which the fallbacks carry too:
            // the jumps from one command's prompt to the next, as Ghostty's jump_to_prompt binds them. Bound
            // nowhere in the defaults when they were added -- Shift alone scrolls a line, Alt+Shift focuses a
            // pane, Ctrl+Alt swaps one, Ctrl+Shift+Alt resizes one. On the primary screen only, where the
            // prompts are; an alternate-screen application keeps the chord.
            fallback(All, CtrlShift, Key::UpArrow, PrimaryScreenOnly, { actions::ScrollMarkUp {} }),
            fallback(All, CtrlShift, Key::DownArrow, PrimaryScreenOnly, { actions::ScrollMarkDown {} }),
            // The chords that move keyboard focus through a window's chrome (Native2026 only, so a Classic
            // user's Ctrl+F6 still reaches the shell). A bare F6 is never bound: mc, vim and htop use it.
            // macOS: Control+F6 between areas (Shift reversed) and Control+F5 for the toolbar -- the chords
            // AppKit's own "move focus to the toolbar" and "next window" use; our toolbar is drawn by QML, so
            // the app answers them. Elsewhere the canvas names Ctrl+F6 alone, so there is no toolbar chord.
            native2026Only(written(All, Ctrl, Key::F6, Any, { actions::FocusNextArea {} })),
            native2026Only(written(All, CtrlShift, Key::F6, Any, { actions::FocusPreviousArea {} })),
            native2026Only(written(PlatformScope::MacOnly, Ctrl, Key::F5, Any, { actions::FocusToolbar {} })),
            // The chords that act on the focused command block: Ctrl+Enter and Shift+F10 open its menu, Enter
            // folds it, Escape lets go of it. Every one requires BlockFocus, so without a focused block the
            // key reaches the application as it always did.
            both(All, Ctrl, Key::Enter, RequiresBlockFocus, { actions::OpenBlockMenu {} }),
            both(All, Shift, Key::F10, RequiresBlockFocus, { actions::OpenBlockMenu {} }),
            both(All, NoMods, Key::Enter, RequiresBlockFocus, { actions::ToggleFold {} }),
            both(All, NoMods, Key::Escape, BlockFocusOutsideSelect, { actions::ClearCommandBlockFocus {} }),
            // macOS: Command+Up/Down step to the previous and next command, Shift+Command+Up/Down do so and
            // select its output. Fallbacks, on the primary screen only, where an application keeps the chord
            // on the alternate one.
            fallback(PlatformScope::MacOnly,
                     Super,
                     Key::UpArrow,
                     PrimaryScreenOnly,
                     { actions::FocusPreviousCommand {} }),
            fallback(PlatformScope::MacOnly,
                     Super,
                     Key::DownArrow,
                     PrimaryScreenOnly,
                     { actions::FocusNextCommand {} }),
            fallback(PlatformScope::MacOnly,
                     SuperShift,
                     Key::UpArrow,
                     PrimaryScreenOnly,
                     { actions::SelectPreviousCommandOutput {} }),
            fallback(PlatformScope::MacOnly,
                     SuperShift,
                     Key::DownArrow,
                     PrimaryScreenOnly,
                     { actions::SelectNextCommandOutput {} }),
            // macOS word and line motion, as every Mac terminal gives it: Option+Left/Right send the shell's
            // backward-word and forward-word (ESC b, ESC f), Command+Left/Right its line start and end (^A,
            // ^E), and Command+Backspace deletes to the line start (^U). On the primary screen only: an
            // alternate-screen application gets the chord itself (Alt+Left is CSI 1;3 D), as it
            // does elsewhere. Fallbacks, so an explicit binding of the chord still wins.
            fallback(
                MacOnly, Alt, Key::LeftArrow, PrimaryScreenOnly, { actions::SendChars { .chars = "\033b" } }),
            fallback(MacOnly,
                     Alt,
                     Key::RightArrow,
                     PrimaryScreenOnly,
                     { actions::SendChars { .chars = "\033f" } }),
            fallback(MacOnly,
                     Super,
                     Key::LeftArrow,
                     PrimaryScreenOnly,
                     { actions::SendChars { .chars = "\001" } }),
            fallback(MacOnly,
                     Super,
                     Key::RightArrow,
                     PrimaryScreenOnly,
                     { actions::SendChars { .chars = "\005" } }),
            fallback(MacOnly,
                     Super,
                     Key::Backspace,
                     PrimaryScreenOnly,
                     { actions::SendChars { .chars = "\025" } }),
            // The chords that move between panes (and zoom one) and move the active tab. Alt+Shift+Arrow, not
            // a bare Alt+Arrow: that one is the shell's word motion. Some desktops and layout switchers grab
            // Alt+Shift; a binding of the chord in input_mapping: wins over these rows. On a Mac, the Command
            // chords iTerm2 and Terminal users already know.
            both(DesktopOnly, AltShift, Key::LeftArrow, Any, { actions::FocusPaneLeft {} }),
            both(DesktopOnly, AltShift, Key::RightArrow, Any, { actions::FocusPaneRight {} }),
            both(DesktopOnly, AltShift, Key::UpArrow, Any, { actions::FocusPaneUp {} }),
            both(DesktopOnly, AltShift, Key::DownArrow, Any, { actions::FocusPaneDown {} }),
            both(DesktopOnly, CtrlShift, Key::PageUp, Any, { actions::MoveTabToLeft {} }),
            both(DesktopOnly, CtrlShift, Key::PageDown, Any, { actions::MoveTabToRight {} }),
            both(PlatformScope::MacOnly, SuperAlt, Key::LeftArrow, Any, { actions::FocusPaneLeft {} }),
            both(PlatformScope::MacOnly, SuperAlt, Key::RightArrow, Any, { actions::FocusPaneRight {} }),
            both(PlatformScope::MacOnly, SuperAlt, Key::UpArrow, Any, { actions::FocusPaneUp {} }),
            both(PlatformScope::MacOnly, SuperAlt, Key::DownArrow, Any, { actions::FocusPaneDown {} }),
            both(PlatformScope::MacOnly, SuperShift, Key::Enter, Any, { actions::TogglePaneZoom {} }),
            both(PlatformScope::MacOnly, Super, Key::PageUp, Any, { actions::MoveTabToLeft {} }),
            both(PlatformScope::MacOnly, Super, Key::PageDown, Any, { actions::MoveTabToRight {} }),
        };
        return rows;
    }

    /// The character rows. A Ctrl+Shift+letter default is a Primary row; a row carrying Alt exists on the
    /// desktop only, since on macOS Option composes characters and Alt+letter cannot fire.
    [[nodiscard]] std::vector<DefaultBindingRow<char32_t>> const& charRows()
    {
        static auto const rows = std::vector<DefaultBindingRow<char32_t>> {
            written(DesktopOnly, CtrlAlt, U',', Any, { actions::ToggleInputMethodHandling {} }),
            written(DesktopOnly, CtrlShift, U'-', Any, { actions::DecreaseFontSize {} }),
            written(DesktopOnly, CtrlShift, U'_', Any, { actions::DecreaseFontSize {} }),
            written(All, Primary, U'N', Any, { actions::NewTerminal {} }),
            written(DesktopOnly, CtrlAlt, U'V', Any, { actions::PasteClipboard { .strip = true } }),
            written(All, Primary, U'V', Any, { actions::PasteClipboard { .strip = false } }),
            written(DesktopOnly, CtrlAlt, U'S', Any, { actions::ScreenshotVT {} }),
            written(DesktopOnly, Ctrl, U'0', Any, { actions::ResetFontSize {} }),
            written(All, Primary, U'T', Any, { actions::CreateNewTab {} }),
            written(DesktopOnly, Primary, U'S', Any, { actions::SpeakSelection {} }),
            written(DesktopOnly, Primary, U'X', Any, { actions::ClosePane {} }),
            written(All, PrimaryShift, U'P', Any, { actions::OpenCommandPalette {} }),
            written(All, CtrlShift, U'\\', Any, { actions::ToggleSplitOrientation {} }),
            written(DesktopOnly, Primary, U'Z', Any, { actions::TogglePaneZoom {} }),
            written(DesktopOnly, Alt, U'1', Any, { actions::SwitchToTab { .position = 1 } }),
            written(DesktopOnly, Alt, U'2', Any, { actions::SwitchToTab { .position = 2 } }),
            written(DesktopOnly, Alt, U'3', Any, { actions::SwitchToTab { .position = 3 } }),
            written(DesktopOnly, Alt, U'4', Any, { actions::SwitchToTab { .position = 4 } }),
            written(DesktopOnly, Alt, U'5', Any, { actions::SwitchToTab { .position = 5 } }),
            written(DesktopOnly, Alt, U'6', Any, { actions::SwitchToTab { .position = 6 } }),
            written(DesktopOnly, Alt, U'7', Any, { actions::SwitchToTab { .position = 7 } }),
            written(DesktopOnly, Alt, U'8', Any, { actions::SwitchToTab { .position = 8 } }),
            written(DesktopOnly, Alt, U'9', Any, { actions::SwitchToTab { .position = 9 } }),
            written(DesktopOnly, Alt, U'0', Any, { actions::SwitchToTab { .position = 10 } }),
            written(
                All, Ctrl, U'C', SelectAndInsert, { actions::CopySelection {}, actions::CancelSelection {} }),
            written(All,
                    Ctrl,
                    U'V',
                    SelectAndInsert,
                    { actions::PasteClipboard { .strip = false }, actions::CancelSelection {} }),
            written(All, CtrlShift, U' ', InsertOnly, { actions::ViNormalMode {} }),
            written(All, CtrlShift, U',', Any, { actions::OpenConfiguration {} }),
            written(All, Primary, U'Q', Any, { actions::Quit {} }),
            written(DesktopOnly, CtrlAlt, U'K', PrimaryScreenOnly, { actions::ScrollMarkUp {} }),
            written(DesktopOnly, CtrlAlt, U'J', PrimaryScreenOnly, { actions::ScrollMarkDown {} }),
            written(DesktopOnly, CtrlAlt, U'O', Any, { actions::OpenFileManager {} }),
            written(DesktopOnly, CtrlAlt, U'.', Any, { actions::ToggleStatusLine {} }),
            written(All, Primary, U'F', Any, { actions::SearchReverse {} }),
            written(All, PrimaryShift, U'H', Any, { actions::NoSearchHighlight {} }),
            written(All,
                    Primary,
                    U'U',
                    Any,
                    { actions::HintMode { .patterns = "url", .hintAction = vtbackend::HintAction::Open } }),
            // Ctrl+Shift+G: kitty's binding for opening the last command's output. Consumed when it declines
            // (nothing finished, blank output, an attached session), as kitty does: the shell would read it
            // as ^G, which makes readline drop the line being typed. On the primary screen only, where the
            // blocks are; an alternate-screen application keeps the chord. Not on macOS, where Command+G
            // steps to the next search match.
            fallback(DesktopOnly,
                     Primary,
                     U'G',
                     PrimaryScreenOnly,
                     { actions::OpenCommandOutput {} },
                     DeclinedInput::Consume),
            // The macOS find chords: Command+F opens the find bar, Command+G and Shift+Command+G step to the
            // next and previous match, Command+E uses the selection. In every mode, so Command+G with no
            // search reaches the session, which opens the bar.
            fallback(PlatformScope::MacOnly, Super, U'F', Any, { actions::SearchReverse {} }),
            fallback(PlatformScope::MacOnly, Super, U'G', Any, { actions::FocusNextSearchMatch {} }),
            fallback(PlatformScope::MacOnly, SuperShift, U'G', Any, { actions::FocusPreviousSearchMatch {} }),
            fallback(PlatformScope::MacOnly, Super, U'E', Any, { actions::SearchSelection {} }),
            // The macOS tab chords: Command+T opens a tab, Command+W closes it, Shift+Command+[ and ] step to
            // the previous and next tab, and Command+1 ... 9 select the tab at that position. Shift+[ arrives
            // as '{': TerminalSession retries a Shift chord under its unshifted base.
            fallback(PlatformScope::MacOnly, Super, U'T', Any, { actions::CreateNewTab {} }),
            fallback(PlatformScope::MacOnly, Super, U'W', Any, { actions::CloseTab {} }),
            fallback(PlatformScope::MacOnly, SuperShift, U'[', Any, { actions::SwitchToTabLeft {} }),
            fallback(PlatformScope::MacOnly, SuperShift, U']', Any, { actions::SwitchToTabRight {} }),
            fallback(PlatformScope::MacOnly, Super, U'1', Any, { actions::SwitchToTab { .position = 1 } }),
            fallback(PlatformScope::MacOnly, Super, U'2', Any, { actions::SwitchToTab { .position = 2 } }),
            fallback(PlatformScope::MacOnly, Super, U'3', Any, { actions::SwitchToTab { .position = 3 } }),
            fallback(PlatformScope::MacOnly, Super, U'4', Any, { actions::SwitchToTab { .position = 4 } }),
            fallback(PlatformScope::MacOnly, Super, U'5', Any, { actions::SwitchToTab { .position = 5 } }),
            fallback(PlatformScope::MacOnly, Super, U'6', Any, { actions::SwitchToTab { .position = 6 } }),
            fallback(PlatformScope::MacOnly, Super, U'7', Any, { actions::SwitchToTab { .position = 7 } }),
            fallback(PlatformScope::MacOnly, Super, U'8', Any, { actions::SwitchToTab { .position = 8 } }),
            fallback(PlatformScope::MacOnly, Super, U'9', Any, { actions::SwitchToTab { .position = 9 } }),
            // The Command chords macOS users expect, none of them one the menu bar owns (Command+Q, H and M).
            // Copy is consumed when nothing is selected, so the chord never reaches the application as a bare
            // letter. Command+plus is Shift+Command+equals on a US layout, which arrives as '+' with Shift
            // and is retried under its unshifted base. Fallbacks, so an explicit binding of the chord still
            // wins.
            fallback(MacOnly,
                     Super,
                     U'C',
                     Any,
                     { actions::CopySelection {}, actions::CancelSelection {} },
                     DeclinedInput::Consume),
            fallback(MacOnly, Super, U'V', Any, { actions::PasteClipboard { .strip = false } }),
            fallback(MacOnly, Super, U'K', Any, { actions::ClearHistoryAndReset {} }),
            fallback(MacOnly, Super, U',', Any, { actions::OpenConfiguration {} }),
            fallback(MacOnly, Super, U'N', Any, { actions::NewTerminal {} }),
            fallback(MacOnly, Super, U'+', Any, { actions::IncreaseFontSize {} }),
            fallback(MacOnly, Super, U'=', Any, { actions::IncreaseFontSize {} }),
            fallback(MacOnly, SuperShift, U'=', Any, { actions::IncreaseFontSize {} }),
            fallback(MacOnly, Super, U'-', Any, { actions::DecreaseFontSize {} }),
            fallback(MacOnly, Super, U'0', Any, { actions::ResetFontSize {} }),
            // The chords that split the active pane: Ctrl+Shift+E and Ctrl+Shift+O on the desktop, Command+D
            // (side by side) and Shift+Command+D (stacked) on a Mac. File > New Window to...: the Connect
            // picker as a sheet; Ctrl+Shift+K is unbound elsewhere, and the BindingCollisions test checks it
            // against every edition and platform.
            both(DesktopOnly, Primary, U'E', Any, { actions::SplitVertical {} }),
            both(DesktopOnly, Primary, U'O', Any, { actions::SplitHorizontal {} }),
            both(DesktopOnly, Primary, U'K', Any, { actions::NewWindowTo {} }),
            both(PlatformScope::MacOnly, Primary, U'D', Any, { actions::SplitVertical {} }),
            both(PlatformScope::MacOnly, PrimaryShift, U'D', Any, { actions::SplitHorizontal {} }),
            both(PlatformScope::MacOnly, PrimaryShift, U'K', Any, { actions::NewWindowTo {} }),
        };
        return rows;
    }

    /// The mouse rows.
    [[nodiscard]] std::vector<DefaultBindingRow<vtbackend::MouseButton>> const& mouseRows()
    {
        using vtbackend::MouseButton;
        static auto const rows = std::vector<DefaultBindingRow<MouseButton>> {
            // Following a link takes Control; the Native2026 edition takes Command on macOS, where Qt's
            // Control/Meta swap is off (@see AA_MacDontSwapCtrlAndMeta).
            classicOnly(
                written(PlatformScope::All, Ctrl, MouseButton::Left, Any, { actions::FollowHyperlink {} })),
            native2026Only(written(
                PlatformScope::DesktopOnly, Ctrl, MouseButton::Left, Any, { actions::FollowHyperlink {} })),
            native2026Only(written(
                PlatformScope::MacOnly, Super, MouseButton::Left, Any, { actions::FollowHyperlink {} })),
            written(PlatformScope::All, NoMods, MouseButton::Middle, Any, { actions::PasteSelection {} }),
            written(PlatformScope::All, NoMods, MouseButton::WheelDown, Any, { actions::ScrollDown {} }),
            written(PlatformScope::All, NoMods, MouseButton::WheelUp, Any, { actions::ScrollUp {} }),
            written(PlatformScope::All, Alt, MouseButton::WheelDown, Any, { actions::DecreaseOpacity {} }),
            written(PlatformScope::All, Alt, MouseButton::WheelUp, Any, { actions::IncreaseOpacity {} }),
            written(PlatformScope::All, Ctrl, MouseButton::WheelDown, Any, { actions::DecreaseFontSize {} }),
            written(PlatformScope::All, Ctrl, MouseButton::WheelUp, Any, { actions::IncreaseFontSize {} }),
            written(PlatformScope::All, Shift, MouseButton::WheelDown, Any, { actions::ScrollPageDown {} }),
            written(PlatformScope::All, Shift, MouseButton::WheelUp, Any, { actions::ScrollPageUp {} }),
            // Fallbacks rather than defaults: every contour.yml written before they existed enumerates the
            // default mouse mappings without them, and would otherwise shadow them away. TerminalSession
            // consults them only AFTER vtbackend has declined the press, so an application that asked for
            // the mouse (vim, tmux) still receives its right-click and its horizontal wheel; and only after
            // the bypass modifier has been stripped, so Shift+Right opens the menu and Shift+wheel switches
            // tabs even then. The horizontal-wheel rows match no modifiers deliberately: sendWheelEvent()
            // transposes the wheel axes while Alt is held (see helper.cpp, transposed()), so Alt+vertical
            // wheel arrives here on the horizontal axis. Carrying Alt, it cannot match these rows, and the
            // Alt+wheel opacity bindings keep working untouched.
            fallback(PlatformScope::All, NoMods, MouseButton::Right, Any, { actions::OpenContextMenu {} }),
            gatedBy(
                fallback(
                    PlatformScope::All, NoMods, MouseButton::WheelLeft, Any, { actions::SwitchToTabLeft {} }),
                [](Config const& config) noexcept { return config.tabSwitchOnHorizontalWheel.value(); }),
            gatedBy(fallback(PlatformScope::All,
                             NoMods,
                             MouseButton::WheelRight,
                             Any,
                             { actions::SwitchToTabRight {} }),
                    [](Config const& config) noexcept { return config.tabSwitchOnHorizontalWheel.value(); }),
            // Ctrl+triple-click selects the output of the command under the pointer, where the terminal would
            // have selected an extended word. On the primary screen only: the alternate screen has no blocks,
            // so there the row would claim the click only to refuse it, and the extended word would be lost.
            withClickCount(
                fallback(PlatformScope::All,
                         Ctrl,
                         MouseButton::Left,
                         PrimaryScreenOnly,
                         { actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Pointer,
                                                         .part = vtbackend::CommandBlockPart::Output } }),
                uint8_t { 3 }),
        };
        return rows;
    }

    /// Whether a row of @p scope exists on @p kind.
    [[nodiscard]] constexpr bool existsOn(PlatformScope scope, KeyboardPlatformKind kind) noexcept
    {
        switch (scope)
        {
            case PlatformScope::All: return true;
            case PlatformScope::MacOnly: return kind == KeyboardPlatformKind::Mac;
            case PlatformScope::DesktopOnly: return kind != KeyboardPlatformKind::Mac;
        }
        return false;
    }

    /// Whether a row of @p scope belongs to @p edition.
    [[nodiscard]] constexpr bool belongsTo(EditionScope scope, DefaultsEdition edition) noexcept
    {
        switch (scope)
        {
            case EditionScope::Both: return true;
            case EditionScope::Native2026Only: return edition == DefaultsEdition::Native2026;
            case EditionScope::ClassicOnly: return edition == DefaultsEdition::Classic;
        }
        return false;
    }

    /// The bindings of @p rows that a configuration of @p edition for @p kind writes.
    template <typename Input>
    [[nodiscard]] std::vector<vtbackend::InputBinding<Input, ActionList>> writtenBindings(
        std::vector<DefaultBindingRow<Input>> const& rows, DefaultsEdition edition, KeyboardPlatformKind kind)
    {
        auto result = std::vector<vtbackend::InputBinding<Input, ActionList>> {};
        for (auto const& row: rows)
            if (row.reach != RowReach::FallbackOnly && existsOn(row.scope, kind)
                && belongsTo(row.edition, edition))
                result.push_back(
                    vtbackend::InputBinding<Input, ActionList> { .modes = row.modes,
                                                                 .modifiers = resolveChord(row.chord, kind),
                                                                 .input = row.input,
                                                                 .binding = row.binding,
                                                                 .clickCount = row.clickCount });
        return result;
    }

    /// The fallback rows of @p rows on @p kind.
    template <typename Input>
    [[nodiscard]] std::vector<FallbackMapping<Input>> fallbackMappings(
        std::vector<DefaultBindingRow<Input>> const& rows, KeyboardPlatformKind kind)
    {
        auto result = std::vector<FallbackMapping<Input>> {};
        for (auto const& row: rows)
            if (row.reach != RowReach::WrittenOnly && existsOn(row.scope, kind))
                result.push_back(
                    FallbackMapping<Input> { .mapping = { .modes = row.modes,
                                                          .modifiers = resolveChord(row.chord, kind),
                                                          .input = row.input,
                                                          .binding = row.binding,
                                                          .clickCount = row.clickCount },
                                             .enabled = row.enabled,
                                             .declined = row.declined });
        return result;
    }

    /// Builds one table per keyboard platform kind with @p build, which is handed the kind.
    template <typename Table, typename Build>
    [[nodiscard]] std::array<Table, KeyboardPlatformKindCount> perKind(Build build)
    {
        auto result = std::array<Table, KeyboardPlatformKindCount> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, KeyboardPlatformKindCount))
            result[index] = build(static_cast<KeyboardPlatformKind>(index));
        return result;
    }
} // namespace

InputMappings makeDefaultInputMappings(DefaultsEdition edition, KeyboardPlatformKind kind)
{
    return InputMappings { .keyMappings = writtenBindings(keyRows(), edition, kind),
                           .charMappings = writtenBindings(charRows(), edition, kind),
                           .mouseMappings = writtenBindings(mouseRows(), edition, kind) };
}

std::vector<FallbackKeyMapping> const& builtinFallbackKeyMappings(KeyboardPlatformKind kind)
{
    static auto const tables =
        perKind<std::vector<FallbackKeyMapping>>([](auto k) { return fallbackMappings(keyRows(), k); });
    return tables[std::to_underlying(kind)];
}

std::vector<FallbackCharMapping> const& builtinFallbackCharMappings(KeyboardPlatformKind kind)
{
    static auto const tables =
        perKind<std::vector<FallbackCharMapping>>([](auto k) { return fallbackMappings(charRows(), k); });
    return tables[std::to_underlying(kind)];
}

std::vector<FallbackMouseMapping> const& builtinFallbackMouseMappings(KeyboardPlatformKind kind)
{
    static auto const tables =
        perKind<std::vector<FallbackMouseMapping>>([](auto k) { return fallbackMappings(mouseRows(), k); });
    return tables[std::to_underlying(kind)];
}

} // namespace contour::config
