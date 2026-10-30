// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// Who put text on the clipboard: what decides whether the frontend tells the user it did.
enum class ClipboardWriter : uint8_t
{
    Program = 0, ///< A program wrote it (OSC 52, kitty's clipboard protocol, a mirrored session's).
    User,        ///< The user copied it (a vi yank, a hint's copy, a copy action).
};

} // namespace vtbackend
