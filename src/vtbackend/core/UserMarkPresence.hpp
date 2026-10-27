// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// Whether a row carries the user's own mark (LineFlag::UserMark).
///
/// In core/ because the gutter's glyph column (screen/) and the block rail's decisions (shell/) both read
/// it, and shell/ may not include screen/.
enum class UserMarkPresence : uint8_t
{
    Absent = 0, ///< The row carries no user mark.
    Present,    ///< vi `mm` marked the row.
};

} // namespace vtbackend
