// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <fmt/format.h>

#include <optional>
#include <string>
#include <variant>

namespace contour::actions
{

// Defines the format to use when extracting a selection range from the terminal.
enum class CopyFormat
{
    // Copies purely the text (with their whitespaces, and newlines, but no formatting).
    Text,

    // Copies the selection in HTML format.
    HTML,

    // Copies the selection in escaped VT sequence format.
    VT,

    // Copies the selection as PNG image.
    PNG,
};

// clang-format off
struct CancelSelection{};
struct ChangeProfile{ std::string name; };
struct ClearHistoryAndReset{};
struct CopyPreviousMarkRange{};
struct CopySelection{ CopyFormat format = CopyFormat::Text; };
struct CreateDebugDump{};
struct DecreaseFontSize{};
struct DecreaseOpacity{};
struct FocusNextSearchMatch{};
struct FocusPreviousSearchMatch{};
struct FollowHyperlink{};
struct IncreaseFontSize{};
struct IncreaseOpacity{};
struct NewTerminal{ std::optional<std::string> profileName; };
struct NoSearchHighlight{};
struct OpenConfiguration{};
struct OpenFileManager{};
struct OpenSelection{};
struct PasteClipboard{ bool strip = false; };
struct PasteSelection{};
struct Quit{};
struct ReloadConfig{ std::optional<std::string> profileName; };
struct ResetConfig{};
struct ResetFontSize{};
struct ScreenshotVT{};
struct ScrollDown{};
struct ScrollMarkDown{};
struct ScrollMarkUp{};
struct ScrollOneDown{};
struct ScrollOneUp{};
struct ScrollPageDown{};
struct ScrollPageUp{};
struct ScrollToBottom{};
struct ScrollToTop{};
struct ScrollUp{};
struct SearchReverse{};
struct SendChars{ std::string chars; };
struct ToggleAllKeyMaps{};
struct ToggleFullscreen{};
struct ToggleInputProtection{};
struct ToggleStatusLine{};
struct ToggleTitleBar{};
struct TraceBreakAtEmptyQueue{};
struct TraceEnter{};
struct TraceLeave{};
struct TraceStep{};
struct ViNormalMode{};
struct WriteScreen{ std::string chars; }; // "\033[2J\033[3J"
// CloseTab
// FocusNextTab
// FocusPreviousTab
// OpenTab
// clang-format on

using Action = std::variant<CancelSelection,
                            ChangeProfile,
                            ClearHistoryAndReset,
                            CopyPreviousMarkRange,
                            CopySelection,
                            CreateDebugDump,
                            DecreaseFontSize,
                            DecreaseOpacity,
                            FocusNextSearchMatch,
                            FocusPreviousSearchMatch,
                            FollowHyperlink,
                            IncreaseFontSize,
                            IncreaseOpacity,
                            NewTerminal,
                            NoSearchHighlight,
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
                            ScrollDown,
                            ScrollMarkDown,
                            ScrollMarkUp,
                            ScrollOneDown,
                            ScrollOneUp,
                            ScrollPageDown,
                            ScrollPageUp,
                            ScrollToBottom,
                            ScrollToTop,
                            ScrollUp,
                            SearchReverse,
                            SendChars,
                            ToggleAllKeyMaps,
                            ToggleFullscreen,
                            ToggleInputProtection,
                            ToggleStatusLine,
                            ToggleTitleBar,
                            TraceBreakAtEmptyQueue,
                            TraceEnter,
                            TraceLeave,
                            TraceStep,
                            ViNormalMode,
                            WriteScreen>;

std::optional<Action> fromString(std::string const& name);

} // namespace contour::actions

// {{{ fmtlib custom formatters
#define DECLARE_ACTION_FMT(T)                                                                    \
    template <>                                                                                  \
    struct fmt::formatter<contour::actions::T>: fmt::formatter<std::string_view>                 \
    {                                                                                            \
        auto format(contour::actions::T const&, format_context& ctx) -> format_context::iterator \
        {                                                                                        \
            return formatter<string_view>::format(#T, ctx);                                      \
        }                                                                                        \
    };

// {{{ declare
DECLARE_ACTION_FMT(CancelSelection)
DECLARE_ACTION_FMT(ChangeProfile)
DECLARE_ACTION_FMT(ClearHistoryAndReset)
DECLARE_ACTION_FMT(CopyPreviousMarkRange)
DECLARE_ACTION_FMT(CopySelection)
DECLARE_ACTION_FMT(CreateDebugDump)
DECLARE_ACTION_FMT(DecreaseFontSize)
DECLARE_ACTION_FMT(DecreaseOpacity)
DECLARE_ACTION_FMT(FocusNextSearchMatch)
DECLARE_ACTION_FMT(FocusPreviousSearchMatch)
DECLARE_ACTION_FMT(FollowHyperlink)
DECLARE_ACTION_FMT(IncreaseFontSize)
DECLARE_ACTION_FMT(IncreaseOpacity)
DECLARE_ACTION_FMT(NewTerminal)
DECLARE_ACTION_FMT(NoSearchHighlight)
DECLARE_ACTION_FMT(OpenConfiguration)
DECLARE_ACTION_FMT(OpenFileManager)
DECLARE_ACTION_FMT(OpenSelection)
DECLARE_ACTION_FMT(PasteClipboard)
DECLARE_ACTION_FMT(PasteSelection)
DECLARE_ACTION_FMT(Quit)
DECLARE_ACTION_FMT(ReloadConfig)
DECLARE_ACTION_FMT(ResetConfig)
DECLARE_ACTION_FMT(ResetFontSize)
DECLARE_ACTION_FMT(ScreenshotVT)
DECLARE_ACTION_FMT(ScrollDown)
DECLARE_ACTION_FMT(ScrollMarkDown)
DECLARE_ACTION_FMT(ScrollMarkUp)
DECLARE_ACTION_FMT(ScrollOneDown)
DECLARE_ACTION_FMT(ScrollOneUp)
DECLARE_ACTION_FMT(ScrollPageDown)
DECLARE_ACTION_FMT(ScrollPageUp)
DECLARE_ACTION_FMT(ScrollToBottom)
DECLARE_ACTION_FMT(ScrollToTop)
DECLARE_ACTION_FMT(ScrollUp)
DECLARE_ACTION_FMT(SearchReverse)
DECLARE_ACTION_FMT(SendChars)
DECLARE_ACTION_FMT(ToggleAllKeyMaps)
DECLARE_ACTION_FMT(ToggleFullscreen)
DECLARE_ACTION_FMT(ToggleInputProtection)
DECLARE_ACTION_FMT(ToggleStatusLine)
DECLARE_ACTION_FMT(ToggleTitleBar)
DECLARE_ACTION_FMT(TraceBreakAtEmptyQueue)
DECLARE_ACTION_FMT(TraceEnter)
DECLARE_ACTION_FMT(TraceLeave)
DECLARE_ACTION_FMT(TraceStep)
DECLARE_ACTION_FMT(ViNormalMode)
DECLARE_ACTION_FMT(WriteScreen)
// }}}
#undef DECLARE_ACTION_FMT

#define HANDLE_ACTION(T)                                                  \
    if (std::holds_alternative<contour::actions::T>(_action))             \
    {                                                                     \
        name = fmt::format("{}", std::get<contour::actions::T>(_action)); \
    }

template <>
struct fmt::formatter<contour::actions::Action>: fmt::formatter<std::string>
{
    auto format(contour::actions::Action const& _action, format_context& ctx) -> format_context::iterator
    {
        std::string name = "Unknown action";
        // {{{ handle
        HANDLE_ACTION(CancelSelection);
        HANDLE_ACTION(ChangeProfile);
        HANDLE_ACTION(ClearHistoryAndReset);
        HANDLE_ACTION(CopyPreviousMarkRange);
        HANDLE_ACTION(CopySelection);
        HANDLE_ACTION(CreateDebugDump);
        HANDLE_ACTION(DecreaseFontSize);
        HANDLE_ACTION(DecreaseOpacity);
        HANDLE_ACTION(FocusNextSearchMatch);
        HANDLE_ACTION(FocusPreviousSearchMatch);
        HANDLE_ACTION(FollowHyperlink);
        HANDLE_ACTION(IncreaseFontSize);
        HANDLE_ACTION(IncreaseOpacity);
        HANDLE_ACTION(NewTerminal);
        HANDLE_ACTION(NoSearchHighlight);
        HANDLE_ACTION(OpenConfiguration);
        HANDLE_ACTION(OpenFileManager);
        HANDLE_ACTION(OpenSelection);
        HANDLE_ACTION(PasteClipboard);
        HANDLE_ACTION(PasteSelection);
        HANDLE_ACTION(Quit);
        HANDLE_ACTION(ReloadConfig);
        HANDLE_ACTION(ResetConfig);
        HANDLE_ACTION(ResetFontSize);
        HANDLE_ACTION(ScreenshotVT);
        HANDLE_ACTION(ScrollDown);
        HANDLE_ACTION(ScrollMarkDown);
        HANDLE_ACTION(ScrollMarkUp);
        HANDLE_ACTION(ScrollOneDown);
        HANDLE_ACTION(ScrollOneUp);
        HANDLE_ACTION(ScrollPageDown);
        HANDLE_ACTION(ScrollPageUp);
        HANDLE_ACTION(ScrollToBottom);
        HANDLE_ACTION(ScrollToTop);
        HANDLE_ACTION(ScrollUp);
        HANDLE_ACTION(SearchReverse);
        HANDLE_ACTION(SendChars);
        HANDLE_ACTION(ToggleAllKeyMaps);
        HANDLE_ACTION(ToggleFullscreen);
        HANDLE_ACTION(ToggleInputProtection);
        HANDLE_ACTION(ToggleStatusLine);
        HANDLE_ACTION(ToggleTitleBar);
        HANDLE_ACTION(TraceBreakAtEmptyQueue);
        HANDLE_ACTION(TraceEnter);
        HANDLE_ACTION(TraceLeave);
        HANDLE_ACTION(TraceStep);
        HANDLE_ACTION(ViNormalMode);
        HANDLE_ACTION(WriteScreen);
        // }}}
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<contour::actions::CopyFormat>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(contour::actions::CopyFormat value, FormatContext& ctx)
    {
        switch (value)
        {
            case contour::actions::CopyFormat::Text: return fmt::format_to(ctx.out(), "Text");
            case contour::actions::CopyFormat::HTML: return fmt::format_to(ctx.out(), "HTML");
            case contour::actions::CopyFormat::PNG: return fmt::format_to(ctx.out(), "PNG");
            case contour::actions::CopyFormat::VT: return fmt::format_to(ctx.out(), "VT");
        }
        crispy::unreachable();
    }
};

#undef HANDLE_ACTION
// ]}}
