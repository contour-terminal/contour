#pragma once

#include <terminal/Cell.h>
#include <terminal/DenseCell.h>

namespace terminal
{

/// Give Cell a proper name. We've to rename Cell later when we're really about to
/// feature different cell implementations in production.
using ThinCell = Cell;

/// Type of cell to be used with the primary screen.
using PrimaryScreenCell = ThinCell;

/// Type of cell to be used with the alternate screen.
using AlternateScreenCell = DenseCell;

/// The Cell to be used with the indicator (and host writable) status line.
using StatusDisplayCell = DenseCell;

} // namespace terminal
