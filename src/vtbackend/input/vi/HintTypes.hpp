// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// Defines the action to perform when a hint is selected.
enum class HintAction : uint8_t
{
    Copy,         ///< Copy matched text to clipboard.
    Open,         ///< Open matched text (e.g. URL in browser).
    Paste,        ///< Paste matched text into the terminal input.
    CopyAndPaste, ///< Copy to clipboard and paste into terminal.
    Select,       ///< Pre-select the match range in visual mode.
};

/// How much of the terminal a hint activation scans.
enum class HintScope : uint8_t
{
    Visible = 0,    ///< The visible page only.
    Scrollback = 1, ///< The visible page plus scrollback history, up to a configured limit.
};

} // namespace vtbackend
