// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/StatusLineExtensions.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include <vtextension/ExtensionLimits.hpp>

namespace vtextension
{

/// The ambient resources an extension's private terminal reads, injected as vtbackend::Terminal's
/// constructor takes them. All borrowed: they must outlive the Surface.
struct SurfaceServices
{
    /// The environment the private terminal reads.
    core::Environment const& environment;

    /// The filesystem the private terminal looks paths up in.
    core::platform::FileSystem const& fileSystem;

    /// The clocks the private terminal reads. @see vtbackend::TerminalClocks
    /// Defaults to the production clocks, as vthost::SessionHost's services do; a test passes its own.
    vtbackend::TerminalClocks clocks = vtbackend::TerminalClocks::system();
};

/// What a Surface is, fixed at construction (only the page and cell sizes change later, @see Surface::resize
/// and Surface::setCellPixelSize).
struct SurfaceConfig
{
    /// The surface's size in cells: the status line's width by its row count (one in v1).
    vtbackend::PageSize pageSize;

    /// The size of one cell in pixels, so that pixel-size queries (`CSI 14 t`, `CSI 16 t`) report the
    /// strip the extension is drawn into and images are cut into fragments of the right size.
    vtbackend::ImageSize cellPixelSize;

    /// The most reply bytes held until drained. Replies are kept whole: to fit a new one the oldest are
    /// evicted, a reply larger than this on its own is dropped, and both are counted.
    /// @see ExtensionLimits::maxReplyBytes
    std::size_t replyCapacity = ExtensionLimits {}.maxReplyBytes;
};

/// An extension's private terminal: the scratch screen its render output is parsed into.
///
/// A headless vtbackend::Terminal over a process-less vtpty::ChannelPty. Whatever the private terminal
/// writes back (replies to DA1, CSI 6 n, CSI 14 t, ...) lands in a bounded buffer the extension drains;
/// it never reaches a shell. The private terminal runs no status-line commands, detects no links and
/// shows no status line of its own.
///
/// Thread-safety: a Surface is used from exactly one thread at a time, the extension's worker. It
/// needs no GUI: feeding it parses on the calling thread, Sixel and kitty decoding included.
class Surface
{
  public:
    /// @param config   The surface's geometry and reply bound.
    /// @param services The ambient resources the private terminal reads.
    Surface(SurfaceConfig config, SurfaceServices services);

    Surface(Surface const&) = delete;
    Surface(Surface&&) = delete;
    Surface& operator=(Surface const&) = delete;
    Surface& operator=(Surface&&) = delete;
    ~Surface() = default;

    /// Replaces the surface's content with one rendered frame.
    ///
    /// Every frame is a full frame: before parsing @p vt the private terminal is reset, so that nothing
    /// an earlier frame did reaches this one. The reset is, in order: CAN, which aborts a sequence the
    /// last frame left unterminated; DECSCL 65;1, which restores the VT500 level and 7-bit replies (DECSTR
    /// is ignored at the VT100 level); DECSTR, which restores the modes, margins, rendition and character
    /// sets; autowrap off (DECSTR turns it on), so overlong text is clipped at the last column rather
    /// than wrapped and scrolled away; Sixel scrolling off (DECSTR leaves it), so an image never scrolls
    /// the row away; the whole page erased; the cursor homed.
    /// @param vt The extension's render output, as VT bytes.
    void feed(std::string_view vt);

    /// Takes the replies the private terminal produced since the last call.
    /// @return The replies, oldest first, each one whole; at most SurfaceConfig::replyCapacity bytes.
    [[nodiscard]] std::string drainReplies();

    /// @return How many replies were dropped over this surface's whole life, evicted to make room or
    ///         too large to hold at all. A reply is what one feed() wrote back: every answer to the
    ///         queries of one frame, together.
    [[nodiscard]] uint64_t droppedReplies() const noexcept { return _droppedReplies; }

    /// @return How many bytes those dropped replies held.
    [[nodiscard]] uint64_t droppedReplyBytes() const noexcept { return _droppedReplyBytes; }

    /// Resizes the surface, keeping the cell's pixel size.
    /// @param pageSize The new size in cells.
    void resize(vtbackend::PageSize pageSize);

    /// Changes the size of one cell in pixels, which a font or pixel-density change of the terminal the
    /// surface is shown in brings: what pixel queries report and images are cut into from the next frame.
    /// @param cellPixelSize One cell of the terminal the surface is shown in, in device pixels.
    void setCellPixelSize(vtbackend::ImageSize cellPixelSize);

    /// @return The surface's current size in cells.
    [[nodiscard]] vtbackend::PageSize pageSize() const noexcept { return _terminal.totalPageSize(); }

    /// Captures what the surface shows now.
    /// @param generation Which render this frame came from; copied into the snapshot.
    /// @return Every row of the private page, and its top row as text and SGR.
    [[nodiscard]] vtbackend::StatusLineExtensionSnapshot snapshot(uint64_t generation) const;

  private:
    /// The ChannelPty's write sink, called once per write: one reply. Keeps @p reply whole, evicting the
    /// oldest replies until it fits; a reply larger than the whole capacity is dropped instead. A reply
    /// is never cut, so the buffer only ever holds complete sequences.
    void appendReply(std::string_view reply);

    std::size_t _replyCapacity;
    std::deque<std::string> _replies; ///< Oldest first.
    std::size_t _replyBytes = 0;      ///< The bytes _replies holds.
    uint64_t _droppedReplies = 0;
    uint64_t _droppedReplyBytes = 0;
    vtbackend::Terminal::NullEvents _events;
    vtbackend::Terminal _terminal; ///< Last: it borrows _events, and its PTY writes into _replies.
};

} // namespace vtextension
