/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2022 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <terminal/cell/DenseCell.h>
#include <terminal/cell/ThinCell.h>

namespace terminal
{

/// Type of cell to be used with the primary screen.
using PrimaryScreenCell = ThinCell;

/// Type of cell to be used with the alternate screen.
using AlternateScreenCell = ThinCell;

/// The Cell to be used with the indicator (and host writable) status line.
using StatusDisplayCell = DenseCell;

} // namespace terminal
