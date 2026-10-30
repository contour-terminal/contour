// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// Whether a paste from the clipboard is stripped first.
enum class PasteStrip : uint8_t
{
    Keep = 0, ///< Pasted as it is.
    Strip,    ///< Runs of white space become one space, and line breaks spaces.
};

} // namespace vtbackend
