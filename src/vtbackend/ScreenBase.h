// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/Cursor.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Line.h>
#include <vtbackend/Sequence.h>

#include <crispy/algorithm.h>
#include <crispy/logstore.h>
#include <crispy/size.h>
#include <crispy/utils.h>

#include <memory>
#include <optional>
#include <string>

namespace vtbackend
{

// A cell type independant minimal representation of Screen.
//
// @see Screen<CellType>
class ScreenBase: public SequenceHandler
{
  public:
    virtual void verifyState() const = 0;
    virtual void fail(std::string const& message) const = 0;

    [[nodiscard]] Cursor& cursor() noexcept { return _cursor; }
    [[nodiscard]] Cursor const& cursor() const noexcept { return _cursor; }
    [[nodiscard]] Cursor const& savedCursorState() const noexcept { return _savedCursor; }
    void resetSavedCursorState() { _savedCursor = {}; }
    virtual void saveCursor() = 0;
    virtual void restoreCursor() = 0;
    virtual void reportColorPaletteUpdate() = 0;

    [[nodiscard]] virtual bool contains(CellLocation coord) const noexcept = 0;
    [[nodiscard]] virtual bool isCellEmpty(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual bool compareCellTextAt(CellLocation position,
                                                 char32_t codepoint) const noexcept = 0;
    [[nodiscard]] virtual std::string cellTextAt(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual CellFlags cellFlagsAt(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual LineFlags lineFlagsAt(LineOffset line) const noexcept = 0;
    virtual void enableLineFlags(LineOffset lineOffset, LineFlags flags, bool enable) noexcept = 0;
    [[nodiscard]] virtual bool isLineFlagEnabledAt(LineOffset line, LineFlags flags) const noexcept = 0;
    [[nodiscard]] virtual std::string lineTextAt(LineOffset line,
                                                 bool stripLeadingSpaces = true,
                                                 bool stripTrailingSpaces = true) const noexcept = 0;
    [[nodiscard]] virtual bool isLineEmpty(LineOffset line) const noexcept = 0;
    [[nodiscard]] virtual uint8_t cellWidthAt(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual LineCount historyLineCount() const noexcept = 0;
    [[nodiscard]] virtual HyperlinkId hyperlinkIdAt(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<HyperlinkInfo const> hyperlinkAt(
        CellLocation pos) const noexcept = 0;
    virtual void inspect(std::string const& message, std::ostream& os) const = 0;
    virtual void moveCursorTo(LineOffset line, ColumnOffset column) = 0; // CUP
    virtual void updateCursorIterator() noexcept = 0;

    [[nodiscard]] virtual std::optional<CellLocation> search(std::u32string_view searchText,
                                                             CellLocation startPosition) = 0;
    [[nodiscard]] virtual std::optional<CellLocation> searchReverse(std::u32string_view searchText,
                                                                    CellLocation startPosition) = 0;

  protected:
    Cursor _cursor {};
    Cursor _savedCursor {};
};

} // namespace vtbackend
