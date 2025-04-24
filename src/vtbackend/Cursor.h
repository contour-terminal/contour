// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Charset.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/primitives.h>

namespace vtbackend
{

/// Terminal cursor data structure.
///
/// NB: Take care what to store here, as DECSC/DECRC will save/restore this struct.
struct Cursor
{
    CellLocation position { .line = LineOffset(0), .column = ColumnOffset(0) };
    bool autoWrap = true; // false;
    bool originMode = false;
    bool wrapPending = false;
    GraphicsAttributes graphicsRendition {};
    CharsetMapping charsets {};
    HyperlinkId hyperlink {};
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    // TODO: CharacterSet for GL and GR
};

} // namespace vtbackend
