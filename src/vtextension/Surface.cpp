// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Surface.hpp>

#include <vtpty/ChannelPty.hpp>

#include <memory>
#include <ranges>
#include <utility>

namespace vtextension
{

namespace
{
    /// What every frame is parsed after. @see Surface::feed
    ///
    /// CAN first: whatever state the parser was left in, it returns to ground, so what follows is read
    /// as written. DECSCL next: DECSTR is not honoured at the VT100 level, and 8-bit replies (S8C1T)
    /// survive it, so a frame that selected either would weaken every later reset; DECSCL is answered at
    /// every level. DECSTR then resets the rendition before the erase, which paints with the pen in
    /// effect.
    constexpr auto FrameReset = std::string_view { "\x18"         // CAN: abort an unterminated sequence
                                                   "\033[65;1\"p" // DECSCL: VT500 level, 7-bit replies
                                                   "\033[!p"      // DECSTR: soft reset
                                                   "\033[?7l"     // DECAWM off: clip, never wrap
                                                   "\033[?80h"    // DECSDM set: no Sixel scrolling
                                                   "\033[2J"      // ED 2: erase the page
                                                   "\033[H" };    // CUP: home

    /// The private terminal's settings: the surface's page, no scrollback, images on, and no status line
    /// (there is none inside a status line).
    /// @param config The surface's configuration.
    /// @return The settings the private terminal is built from.
    vtbackend::Settings surfaceSettings(SurfaceConfig const& config)
    {
        auto settings = vtbackend::Settings {};
        settings.pageSize = config.pageSize;
        settings.historyLimits = vtbackend::HistoryLimits {};
        settings.statusDisplayType = vtbackend::StatusDisplayType::None;
        settings.goodImageProtocol = true;
        // A Sixel image stops at the bottom margin rather than scrolling the one-row page away.
        settings.sixelScrolling = vtbackend::SixelScrolling::Disabled;
        return settings;
    }

    /// The private terminal's PTY: its writes are the surface's replies, and go nowhere else.
    /// @param pageSize The surface's page size.
    /// @param sink Where the terminal's writes go.
    /// @return The device.
    std::unique_ptr<vtpty::Pty> replyChannel(vtbackend::PageSize pageSize, vtpty::ChannelPty::WriteSink sink)
    {
        auto pty = std::make_unique<vtpty::ChannelPty>(pageSize);
        pty->setWriteSink(std::move(sink));
        return pty;
    }
} // namespace

Surface::Surface(SurfaceConfig config, SurfaceServices services):
    _replyCapacity { config.replyCapacity },
    _terminal { _events,
                services.environment,
                services.fileSystem,
                replyChannel(config.pageSize, [this](std::string_view reply) { appendReply(reply); }),
                surfaceSettings(config),
                std::nullopt,
                services.clocks,
                // Mode 2034 reader sessions authenticate a reader against the shell; a surface has none.
                [] { return vtbackend::SemanticBlockToken {}; },
                std::nullopt }
{
    // Terminal takes no cell size at construction; until it is set, pixel queries report zero and images
    // cannot be cut into cells. Set once here, before the surface is handed out.
    _terminal.setCellPixelSize(config.cellPixelSize);
}

void Surface::feed(std::string_view vt)
{
    _terminal.writeToScreen(FrameReset);
    _terminal.writeToScreen(vt);
    // Replies are queued while parsing; flushing writes them through the ChannelPty into _replies.
    _terminal.flushInput();
}

std::string Surface::drainReplies()
{
    auto replies = std::string {};
    replies.reserve(_replyBytes);
    for (auto const& reply: _replies)
        replies += reply;
    _replies.clear();
    _replyBytes = 0;
    return replies;
}

void Surface::resize(vtbackend::PageSize pageSize)
{
    _terminal.resizeScreenKeepingCellSize(pageSize);
}

void Surface::setCellPixelSize(vtbackend::ImageSize cellPixelSize)
{
    _terminal.setCellPixelSize(cellPixelSize);
}

vtbackend::StatusLineExtensionSnapshot Surface::snapshot(uint64_t generation) const
{
    auto const& grid = _terminal.currentScreen().grid();
    auto lines = std::vector<vtbackend::Line> {};
    lines.reserve(grid.pageSize().lines.as<size_t>());
    for (auto const row: std::views::iota(0, grid.pageSize().lines.as<int>()))
    {
        // The frame outlives this terminal (it is read on other threads, and kept after a fault drops the
        // surface), so its images are re-created over the same data without the pool's remover, which
        // would call back into this terminal when the last of them goes.
        auto& line = lines.emplace_back(grid.lineAt(vtbackend::LineOffset(row)));
        vtbackend::rehomeImageFragments(line, &vtbackend::detachedImage);
    }

    auto sgrText = std::string {};
    if (!lines.empty())
    {
        // Extend mode carries no graphics: with its fragments gone an image cell is a blank (text drawn
        // over an image stays), and trailing ones trim away at the end.
        auto textOnly = lines.front();
        textOnly.storage().imageFragments.reset();
        sgrText = textOnly.toUtf8WithSgr(vtbackend::ColumnOffset(0),
                                         vtbackend::ColumnOffset::cast_from(textOnly.trimmedColumns()));
    }

    return { .lines = std::move(lines), .sgrText = std::move(sgrText), .generation = generation };
}

void Surface::appendReply(std::string_view reply)
{
    if (reply.size() > _replyCapacity)
    {
        ++_droppedReplies;
        _droppedReplyBytes += reply.size();
        return;
    }
    while (_replyBytes + reply.size() > _replyCapacity)
    {
        _replyBytes -= _replies.front().size();
        _droppedReplyBytes += _replies.front().size();
        ++_droppedReplies;
        _replies.pop_front();
    }
    _replies.emplace_back(reply);
    _replyBytes += reply.size();
}

} // namespace vtextension
