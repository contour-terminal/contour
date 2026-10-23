// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtextension
{

/// Where an extension's surface is shown.
enum class Placement : uint8_t
{
    /// A `{Extension:…}` segment inside the indicator status line: text and SGR only.
    Segment,
    /// The extension draws the whole status line, graphics included.
    TakeOver,
};

} // namespace vtextension
