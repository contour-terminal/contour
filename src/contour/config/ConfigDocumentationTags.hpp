// SPDX-License-Identifier: Apache-2.0
#pragma once

// The names of the configuration's documentation entries, without their text.
//
// A ConfigEntry names one of these tags; the strings a tag stands for are defined in ConfigDocumentation.hpp,
// which only the code that writes them out includes. A file that reads a setting parses these few lines, and
// an edit to a documentation string rebuilds the writers alone. A new entry is a tag here and a Text
// specialization there.

namespace contour::config::documentation
{

/// The text of the documentation entry @p Tag names: two string_views, `Config` (the commented contour.yml
/// text) and `Web` (the website's Markdown). ConfigDocumentation.hpp specializes it once per tag; a tag
/// without a specialization is a compile error wherever its text is read.
template <typename Tag>
struct Text;

// clang-format off
struct CopyCommandPreserveMultiline {};
struct Shell {};
struct EscapeSandbox {};
struct SshHostConfig {};
struct Maximized {};
struct Fullscreen {};
struct ShowTitleBar {};
struct WindowShadow {};
struct DimUnfocused {};
struct ShowIndicatorOnResize {};
struct Mouse {};
struct SearchCaseSensitivity {};
struct InsertAfterYank {};
struct CopyLastMarkRangeOffset {};
struct WMClass {};
struct TabLabel {};
struct TabColor {};
struct PixelReporting {};
struct AccessibilityAnnouncements {};
struct AccessibilityCaretReporting {};
struct HyperlinkHoverTooltip {};
struct ProgressTimeout {};
struct NotificationCloseTimeout {};
struct TabBarPosition {};
struct TabBarVisibility {};
struct UiStyle {};
struct ChromeDensity {};
struct SecureInput {};
struct WindowControlStyle {};
struct UiFontFamily {};
struct UiFontSize {};
struct Margins {};
struct TerminalSize {};
struct TerminalId {};
struct History {};
struct Folding {};
struct Gutter {};
struct OscContext {};
struct CommandBlocks {};
struct Scrollbar {};
struct StickyHeader {};
struct NotifyOnCommandFinish {};
struct StatusLine {};
struct OptionKeyAsAlt {};
struct Fonts {};
struct Permissions {};
struct DrawBoldTextWithBrightColors {};
struct BlinkStyle {};
struct ScreenTransitionStyle {};
struct ScreenTransitionDuration {};
struct CursorMotionAnimationDuration {};
struct Colors {};
struct ModalCursorScrollOff {};
struct ModeInsert {};
struct ModeNormal {};
struct ModeVisual {};
struct SmoothLineScrolling {};
struct SmoothScrolling {};
struct MomentumScrolling {};
struct HighlightTimeout {};
struct HighlightDoubleClickerWord {};
struct Background {};
struct Bell {};
struct FrozenDecMode {};
struct Live {};
struct DefaultsEdition {};
struct PlatformPlugin {};
struct Renderer {};
struct CommandPaletteRecentCount {};
struct PTYReadBufferSize {};
struct PTYBufferObjectSize {};
struct ReflowOnResize {};
struct SnapToRows {};
struct ConnectPickerOnStart {};
struct RememberWindowPosition {};
struct TabSwitchOnHorizontalWheel {};
struct GuiConfigLocked {};
struct Theme {};
struct TextScalingMethod {};
struct GraphemeClustering {};
struct ColorSchemes {};
struct Profiles {};
struct DefaultProfiles {};
struct Layouts {};
struct Hosts {};
struct DefaultLayout {};
struct WordDelimiters {};
struct ExtendedWordDelimiters {};
struct BypassMouseProtocolModifiers {};
struct OnMouseSelection {};
struct MouseBlockSelectionModifiers {};
struct InputMappings {};
struct SpawnNewProcess {};
struct EarlyExitThreshold {};
struct Images {};
struct Daemon {};
struct QuickTerminal {};
struct ExperimentalFeatures {};
struct DefaultColors {};
struct HyperlinkDecoration {};
struct LinkDetection {};
struct FileOpener {};
struct HyperlinkDecorationColors {};
struct YankHighlight {};
struct NormalModeCursorline {};
struct Selection {};
struct SearchHighlight {};
struct SearchHighlightFocused {};
struct WordHighlightCurrent {};
struct WordHighlight {};
struct HintLabel {};
struct HintLabelTyped {};
struct HintMatch {};
struct HintPatterns {};
struct Extensions {};
struct HintScrollbackLines {};
struct IndicatorStatusLine {};
struct FoldMarker {};
struct FoldMarkerHeader {};
struct FoldMarkerDefault {};
struct FoldMarkerHover {};
struct ContextTint {};
struct ContextTintHeader {};
struct BlockStatus {};
struct BlockStatusHeader {};
struct BlockStatusHover {};
struct BlockStatusHoverHeader {};
struct GutterText {};
struct GutterTextSet {};
struct IndentedEntry {};
struct StickyHeaderColors {};
struct StickyHeaderColorsHeader {};
struct InputMethodEditor {};
struct InputMethodEditorSupport {};
struct InputMethodEditorStyle {};
struct NormalColors {};
struct BrightColors {};
struct DimColors {};
struct GitDrawings {};
struct BoxArcStyle {};
struct BrailleStyle {};
// clang-format on

} // namespace contour::config::documentation
