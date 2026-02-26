// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/cell/CompactCell.h>
#include <vtbackend/cell/SimpleCell.h>

namespace vtbackend
{

/// Type of cell to be used with the primary screen.
using PrimaryScreenCell = CompactCell;

/// Type of cell to be used with the alternate screen.
using AlternateScreenCell = CompactCell;

/// Type of cell to be used with the HUD (Heads-Up Display) overlay screen.
using HudScreenCell = CompactCell;

/// The Cell to be used with the indicator (and host writable) status line.
using StatusDisplayCell = SimpleCell;

} // namespace vtbackend
