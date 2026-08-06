// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `PaneSink` — where one mirrored tmux pane's bytes and geometry go.
///
/// The headless/TTY mirror backs each pane with a replay terminal
/// (`PaneView`); the GUI backs it with the pane session's ChannelPty so the
/// session's own parser emulates. Injecting the sink keeps
/// `TmuxClientModel`'s replay bookkeeping (capture-pane ordering, pending
/// output buffering) identical for both.

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace vthost::tmux
{

/// Receives one remote pane's byte stream and layout extents.
class PaneSink
{
  public:
    virtual ~PaneSink() = default;

    /// Feeds raw remote bytes (live %output or capture-pane replay).
    virtual void feed(std::string_view bytes) = 0;

    /// Applies a new layout extent for the pane.
    virtual void resize(int columns, int lines) = 0;
};

/// Creates the sink backing a newly discovered pane.
using PaneSinkFactory = std::function<std::unique_ptr<PaneSink>(uint64_t pane, int columns, int lines)>;

} // namespace vthost::tmux
