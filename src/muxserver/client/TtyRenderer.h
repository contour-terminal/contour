// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Renders a `RemoteScreen` into a VT byte stream for a real terminal — the
/// thin-client display path of `contour attach`. Pure functions: bytes in,
/// bytes out, golden-testable without a TTY.

#include <string>

#include <muxserver/client/AttachClient.h>
#include <muxserver/proto/Pdu.h>

namespace muxserver::client
{

/// The SGR sequence selecting @p cell's rendition (starting from a reset).
/// @param cell The cell whose attributes to select.
/// @return The `ESC [ ... m` byte sequence.
[[nodiscard]] std::string sgrFor(proto::WireCell const& cell);

/// The SGR sequence selecting @p line's fill attributes (starting from a
/// reset) — what a blank region of that row is colored with.
/// @param line The row whose fill colors to select.
/// @return The `ESC [ ... m` byte sequence.
[[nodiscard]] std::string sgrForFill(proto::WireLine const& line);

/// One full repaint of @p screen's viewport: cursor hidden, every row
/// repainted in place, then the cursor restored to the remote position.
/// @param screen The mirrored screen to draw.
/// @return The VT byte stream to write to the local terminal.
[[nodiscard]] std::string renderViewport(RemoteScreen const& screen);

} // namespace muxserver::client
