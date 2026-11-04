// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/GraphicsAttributes.hpp>
#include <vtbackend/core/Hyperlink.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <expected>

namespace vtbackend
{

/// Everything a Cursor holds, as plain data.
struct CursorState
{
    CellLocation position {};                ///< @see Cursor::position
    bool originMode = false;                 ///< @see Cursor::originMode
    bool wrapPending = false;                ///< @see Cursor::wrapPending
    GraphicsAttributes graphicsRendition {}; ///< @see Cursor::graphicsRendition
    CharsetState charsets {};                ///< @see Cursor::charsets
    HyperlinkId hyperlink {};                ///< @see Cursor::hyperlink

    bool operator==(CursorState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.position);
        visitor(2, self.originMode);
        visitor(3, self.wrapPending);
        visitor(4, self.graphicsRendition);
        visitor(5, self.charsets);
        visitor(6, self.hyperlink);
    }
};

/// Terminal cursor data structure.
///
/// NB: Take care what to store here, as DECSC/DECRC will save/restore this struct.
struct Cursor
{
    CellLocation position { .line = LineOffset(0), .column = ColumnOffset(0) };
    bool originMode = false;
    bool wrapPending = false;
    GraphicsAttributes graphicsRendition {};
    CharsetMapping charsets {};
    HyperlinkId hyperlink {};
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    // TODO: CharacterSet for GL and GR

    /// @return Everything the cursor holds.
    [[nodiscard]] CursorState captureState() const noexcept
    {
        return CursorState { .position = position,
                             .originMode = originMode,
                             .wrapPending = wrapPending,
                             .graphicsRendition = graphicsRendition,
                             .charsets = charsets.captureState(),
                             .hyperlink = hyperlink };
    }

    /// Replaces the cursor with @p state.
    /// @param state A captured cursor.
    /// @return Nothing, or why @p state is not a cursor; the cursor is untouched then.
    [[nodiscard]] std::expected<void, StateDecodeError> restoreState(CursorState const& state) noexcept
    {
        auto restoredCharsets = CharsetMapping {};
        if (auto const done = restoredCharsets.restoreState(state.charsets); !done)
            return done;
        position = state.position;
        originMode = state.originMode;
        wrapPending = state.wrapPending;
        graphicsRendition = state.graphicsRendition;
        charsets = restoredCharsets;
        hyperlink = state.hyperlink;
        return {};
    }
};

} // namespace vtbackend
