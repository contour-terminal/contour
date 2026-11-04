// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/graphics/regis/ReGISState.hpp>
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/screen/Cursor.hpp>
#include <vtbackend/shell/LivePromptMarks.hpp>
#include <vtbackend/shell/PromptRegion.hpp>
#include <vtbackend/state/GridState.hpp>
#include <vtbackend/state/ImageState.hpp>
#include <vtbackend/state/StateCodec.hpp>
#include <vtbackend/vt/SemanticPrompt.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace vtbackend
{

/// Everything a Screen holds that a later byte or key could observe, as plain data.
///
/// The booleans mirror the Screen's own members and cross the wire as bytes: this is the serialization
/// boundary, where the type of a field is the Screen's, not a new API choice.
struct ScreenState
{
    GridState grid {};          ///< The grid, history included.
    CursorState cursor {};      ///< The cursor.
    CursorState savedCursor {}; ///< What DECSC saved.
    GraphicsAttributes
        savedGraphicsRenditions {};        ///< The rendition the SGR save (XTPUSHSGR-less CSI # {) set aside.
    CellLocation lastCursorPosition {};    ///< @see Screen::_lastCursorPosition
    LineProvenance stamp {};               ///< What rows the cursor reaches are handed.
    uint64_t blockRowChanges = 0;          ///< @see Screen::blockRowChanges
    uint64_t restampWork = 0;              ///< @see Screen::restampWork
    uint64_t linesScrolledIntoHistory = 0; ///< @see Screen::linesScrolledIntoHistory
    std::vector<LivePromptMarks::LineMarks> livePromptMarks {}; ///< The OSC 133 marks of the live prompt.
    PromptRedraw promptRedraw = PromptRedraw::OnResize;         ///< Whether the shell redraws on resize.
    OutputStartMarking outputStartMarking = OutputStartMarking::Unseen; ///< Whether ;C was seen.
    KittyScreenState kitty {};                                          ///< The kitty graphics tables.

    /// The ReGIS interpreter and canvas; absent until the first ReGIS DCS hooked on this screen. A ReGIS
    /// string draws only once it ends (regis::ReGISParser buffers it), so one in flight has drawn nothing
    /// yet: its replay from the terminal's in-flight tail starts from this state as it is.
    std::optional<regis::ReGISState> regis {};

    bool rectangularAttributeMode = true; ///< @see Screen::_rectangularAttributeMode
    bool isoProtectionActive = false;     ///< @see Screen::_isoProtectionActive
    int enableLocalFunctions = 0;         ///< DECELF
    int localFunctionKeyControl = 0;      ///< DECLFKC
    int modifierKeyReporting = 0;         ///< DECSMKR
    CellSizeDropLog cellSizeDropLog = CellSizeDropLog::NotYetLogged; ///< @see Screen::_cellSizeDropLog

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.grid);
        visitor(2, self.cursor);
        visitor(3, self.savedCursor);
        visitor(4, self.savedGraphicsRenditions);
        visitor(5, self.lastCursorPosition);
        visitor(6, self.stamp);
        visitor(7, self.blockRowChanges);
        visitor(8, self.restampWork);
        visitor(9, self.linesScrolledIntoHistory);
        visitor(10, self.livePromptMarks);
        visitor(11, self.promptRedraw);
        visitor(12, self.outputStartMarking);
        visitor(13, self.kitty);
        visitor(14, self.regis);
        visitor(15, self.rectangularAttributeMode);
        visitor(16, self.isoProtectionActive);
        visitor(17, self.enableLocalFunctions);
        visitor(18, self.localFunctionKeyControl);
        visitor(19, self.modifierKeyReporting);
        visitor(20, self.cellSizeDropLog);
    }
};

/// A screen built from its captured state and not yet put in place: what Screen::stageRestore() checked and
/// built, for Screen::commitRestore() to swap in without anything left that can fail.
struct StagedScreen
{
    ScreenState const* state = nullptr;              ///< The state; outlives the staged screen.
    ImageFragmentResolver const* resolver = nullptr; ///< Resolves its kitty tables' images.
    StagedGrid grid;                                 ///< The grid, built.
    Cursor cursor {};                                ///< The cursor, restored.
    Cursor savedCursor {};                           ///< What DECSC saved, restored.
};

} // namespace vtbackend
