// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/ClipboardTarget.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace vtbackend::clipboard_selection
{

/// Decodes the selection parameter of `OSC 52 ; Pc ; Pd ST` into the places the copy lands.
///
/// _Pc_ is a list of letters, as in xterm: `c` the clipboard, `p` the primary selection, `s` the configurable
/// one -- which here, as in most terminals, is the clipboard. An empty _Pc_ is xterm's `s0`, and so the
/// clipboard too, which is what the Clipboard Extension (DA1 `52`) requires of it. Letters naming a place
/// Contour has not got (`q`, the cut buffers `0`--`7`) and letters xterm does not define are skipped, as
/// xterm skips them, so `cp` still reaches the clipboard on a desktop without a primary selection.
///
/// @param pc The selection parameter, as the application sent it.
/// @return The targets it names, or nothing when it names none Contour has: there is nowhere to copy to.
[[nodiscard]] std::optional<ClipboardTargets> parse(std::string_view pc);

/// Encodes @p targets as the selection parameter that parse() reads back as the same set.
///
/// What the daemon puts in a mirrored clipboard write, so the client lands it where the session's
/// application asked.
/// @param targets The targets to name; never empty, as parse() never yields an empty set -- an empty Pc
///                is the clipboard, so an empty set would come back as one.
/// @return The canonical letters, in the order `c`, `p`.
[[nodiscard]] std::string format(ClipboardTargets targets);

} // namespace vtbackend::clipboard_selection
