// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/Search.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/input/vi/HintTypes.hpp>
#include <vtbackend/input/vi/JumpHistory.hpp>
#include <vtbackend/input/vi/Selector.hpp>
#include <vtbackend/input/vi/ViCommands.hpp>
#include <vtbackend/input/vi/ViInputHandler.hpp>
#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/shell/CommandBlockPaths.hpp>
#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/state/StateWire.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vtbackend
{

/// What a ViInputHandler holds, as plain data.
struct ViInputHandlerState
{
    ViMode mode = ViMode::Insert; ///< The mode in force.
    std::string pendingInput {};  ///< The command typed so far.
    uint32_t count = 0;           ///< The repeat count typed so far.
    char32_t lastChar = 0;        ///< The last character it handled.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.mode);
        visitor(2, self.pendingInput);
        visitor(3, self.count);
        visitor(4, self.lastChar);
    }
};

/// What ViCommands holds, as plain data.
struct ViCommandsState
{
    CellLocation cursorPosition {};                   ///< The vi cursor.
    ViMode lastMode = ViMode::Insert;                 ///< The mode the cursor look is restored for.
    CursorShape lastCursorShape = CursorShape::Block; ///< The cursor shape to give back on leaving vi.
    char32_t lastChar = 0;                            ///< The target of the last f/t/F/T.
    std::optional<ViMotion> lastCharMotion {};        ///< Which of them it was.
    bool lastCursorVisible = true;                    ///< The cursor visibility to give back.
    JumpHistoryState jumpHistory {};                  ///< The jump list.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.cursorPosition);
        visitor(2, self.lastMode);
        visitor(3, self.lastCursorShape);
        visitor(4, self.lastChar);
        visitor(5, self.lastCharMotion);
        visitor(6, self.lastCursorVisible);
        visitor(7, self.jumpHistory);
    }
};

/// Which word-boundary helper a selection measures with.
enum class SelectionHelperKind : uint8_t
{
    Standard = 0, ///< Settings::wordDelimiters.
    Extended,     ///< Settings::extendedWordDelimiters.
    Custom,       ///< The delimiters of the last triggerWordWiseSelectionWithCustomDelimiters.
};

template <>
struct detail::StateEnumWire<SelectionHelperKind>
{
    static constexpr auto Values = std::array { SelectionHelperKind::Standard,
                                                SelectionHelperKind::Extended,
                                                SelectionHelperKind::Custom };
};

/// A selection, as plain data.
struct SelectionState
{
    SelectionKind kind = SelectionKind::Linear;                 ///< Its class.
    SelectionHelperKind helper = SelectionHelperKind::Standard; ///< What it measures words with.
    CellLocation from {};                                       ///< Where it started.
    CellLocation to {};                                         ///< Where it reaches.
    Selection::State state = Selection::State::Waiting;         ///< How far the gesture got.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.kind);
        visitor(2, self.helper);
        visitor(3, self.from);
        visitor(4, self.to);
        visitor(5, self.state);
    }
};

/// The terminal's one search, without what is derived from it (the compiled query).
struct SearchState
{
    std::u32string pattern {};                 ///< As the user gave it.
    SearchOptions options {};                  ///< The find bar's toggles.
    SearchOrigin origin = SearchOrigin::Typed; ///< Where the pattern came from.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.pattern);
        visitor(2, self.options);
        visitor(3, self.origin);
    }
};

/// An open hint-mode session, as plain data: what the next filter key, a scroll or a re-scan acts on.
///
/// The pattern table is not here: it is the profile's (Settings::linkPatterns), which the restoring process
/// loads as it loads the palette. The resolver is rebuilt from @ref directory; its per-block memo of path
/// bases is re-derived, being a pure function of the block records and @ref directory.
struct HintSessionState
{
    HintAction action = HintAction::Copy;     ///< What selecting a match does.
    HintScope scope = HintScope::Visible;     ///< Whether a scroll re-scans or shifts the labels.
    std::vector<std::string> patternNames {}; ///< The patterns taking part; empty: all of them.
    std::vector<HintMatch> matches {};        ///< Every match of the session, labelled, in reading order.
    std::string filter {};                    ///< The label prefix typed so far.
    std::optional<ViMode> previousViMode {};  ///< The mode closing the session returns to.
    /// The directory paths resolve against, as it was when the session opened; none where it was refused.
    std::optional<TerminalDirectory> directory {};

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.action);
        visitor(2, self.scope);
        visitor(3, self.patternNames);
        visitor(4, self.matches);
        visitor(5, self.filter);
        visitor(6, self.previousViMode);
        visitor(7, self.directory);
    }
};

/// Vi mode, selection, search, the word delimiters the last custom selection used, and a hint session.
struct InteractionState
{
    ViInputHandlerState viInput {};             ///< The modal input handler.
    ViCommandsState viCommands {};              ///< The vi cursor and its memory.
    std::optional<SelectionState> selection {}; ///< The selection, if any.
    std::string customWordDelimiters {};     ///< For a WordWise selection with SelectionHelperKind::Custom.
    SearchState search {};                   ///< The search.
    std::optional<HintSessionState> hint {}; ///< The open hint-mode session, if any.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.viInput);
        visitor(2, self.viCommands);
        visitor(3, self.selection);
        visitor(4, self.customWordDelimiters);
        visitor(5, self.search);
        visitor(6, self.hint);
    }
};

namespace detail
{
    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, SearchOptions>
    void visitForeignFields(Visitor& visitor, T& options)
    {
        visitor(1, options.caseSensitivity);
        visitor(2, options.syntax);
        visitor(3, options.wordBoundary);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, LinkLocation>
    void visitForeignFields(Visitor& visitor, T& location)
    {
        visitor(1, location.line);
        visitor(2, location.column);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, LinkTarget>
    void visitForeignFields(Visitor& visitor, T& target)
    {
        visitor(1, target.kind);
        visitor(2, target.target);
        visitor(3, target.location);
        visitor(4, target.pathKind);
        visitor(5, target.directory);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, HintMatch>
    void visitForeignFields(Visitor& visitor, T& match)
    {
        visitor(1, match.label);
        visitor(2, match.matchedText);
        visitor(3, match.target);
        visitor(4, match.opening);
        visitor(5, match.start);
        visitor(6, match.end);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, TerminalDirectory>
    void visitForeignFields(Visitor& visitor, T& directory)
    {
        visitor(1, directory.openLocally);
        visitor(2, directory.locality);
    }
} // namespace detail

} // namespace vtbackend
