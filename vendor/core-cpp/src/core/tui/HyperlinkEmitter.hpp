// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file HyperlinkEmitter.hpp
/// @brief Emits OSC 8 framing around a buffer's hyperlink regions while its cells flush.

#include <core/tui/Buffer.hpp>

#include <span>
#include <string_view>

namespace core::tui
{

class TerminalOutput;

/// @brief A view of a buffer's hyperlink regions, for cell lookup during flush.
///
/// A flat scan rather than a row index: a frame carries at most a handful of regions, so bucketing
/// them would cost more bookkeeping than it saves. The scan is never reached on a linkless frame —
/// HyperlinkEmitter::beforeWrite() tests empty() inline first, which is where the win actually is.
class HyperlinkIndex
{
  public:
    /// @brief Builds an empty index that reports every cell as unlinked.
    HyperlinkIndex() = default;

    /// @brief Builds an index over @p buffer's regions.
    /// @param buffer The buffer being flushed. Borrowed, not copied: it must outlive this index,
    ///        which every flush-scoped use satisfies — Screen swaps its buffers only after the
    ///        flush returns, and Buffer::writeTo indexes itself.
    explicit HyperlinkIndex(Buffer const& buffer): _regions(buffer.hyperlinks()) {}

    /// @brief Returns the region covering (@p row, @p col), or nullptr when unlinked.
    [[nodiscard]] HyperlinkRegion const* at(int row, int col) const noexcept;

    /// @brief Returns the URI covering (@p row, @p col), or an empty view when unlinked.
    [[nodiscard]] std::string_view uriAt(int row, int col) const noexcept;

    /// @brief Whether this index holds no regions at all.
    [[nodiscard]] bool empty() const noexcept { return _regions.empty(); }

  private:
    std::span<HyperlinkRegion const> _regions;
};

/// @brief Keeps the terminal's open OSC 8 link in sync with a buffer's regions during flush.
///
/// The link is opened lazily, immediately before the first cell written inside a region, and
/// closed before the first write outside it. Opening lazily is what makes this correct under
/// a cell-diffing renderer: cells the diff skips are never bracketed, so an unchanged link
/// costs nothing, and no cell can be attributed to a link it does not belong to.
///
/// Skipped cells keep the link the terminal recorded when they were last written, which is
/// exactly the link that would have been re-emitted. Detecting a region whose URI changed
/// while its cells did not is the caller's job — compare HyperlinkIndex::uriAt() against the
/// previous frame's index in the diff condition, since Cell equality cannot see an
/// out-of-band URI.
class HyperlinkEmitter
{
  public:
    /// @brief Constructs an emitter writing to @p out for the regions in @p index.
    /// @param out Sink receiving the OSC 8 framing. Must outlive this emitter.
    /// @param index Index over the buffer being flushed. Must outlive this emitter.
    HyperlinkEmitter(TerminalOutput& out, HyperlinkIndex const& index) noexcept;

    /// @brief Closes any still-open link, so framing can never leak past a flush.
    ~HyperlinkEmitter();

    HyperlinkEmitter(HyperlinkEmitter const&) = delete;
    HyperlinkEmitter(HyperlinkEmitter&&) = delete;
    auto operator=(HyperlinkEmitter const&) -> HyperlinkEmitter& = delete;
    auto operator=(HyperlinkEmitter&&) -> HyperlinkEmitter& = delete;

    /// @brief Synchronizes link state with the region covering (@p row, @p col).
    ///
    /// Call immediately before writing that cell's grapheme. A frame with no regions at all —
    /// every fullscreen frame, and every prompt frame with hyperlinks disabled — returns here
    /// without a call or a scan, so the per-cell cost of carrying this mechanism is one inline
    /// test. Gating this at the call sites instead would leave each of them free to forget.
    void beforeWrite(int row, int col)
    {
        if (_index->empty())
            return;
        syncTo(row, col);
    }

    /// @brief Closes any open link. Idempotent.
    ///
    /// Call at the end of a row and before any cursor movement or erase that should not be
    /// attributed to the link.
    void close();

  private:
    /// @brief Opens, closes or leaves alone the link for (@p row, @p col). Out of line: reached
    ///        only on frames that actually carry regions.
    void syncTo(int row, int col);

    TerminalOutput* _out;
    HyperlinkIndex const* _index;
    HyperlinkRegion const* _open = nullptr;
};

/// @brief Per-flush hyperlink state: both frames' regions plus the emitter bracketing writes.
///
/// Bundled so each flush path sets links up in one line and asks one question, rather than
/// repeating the setup and — more importantly — the retargeted() test, which is easy to omit and
/// silently leaves the terminal pointing at a stale target.
struct FlushLinks
{
    HyperlinkIndex current;  ///< The frame being flushed.
    HyperlinkIndex previous; ///< The frame on screen; empty when not diffing.
    HyperlinkEmitter emitter;

    /// Whether either frame has any regions at all. Hoisted because the per-cell retargeted()
    /// test would otherwise run for every unchanged cell of every frame, links or not.
    bool any = false;

    /// @brief Builds the state for one flush.
    /// @param out Sink receiving the framing. Must outlive this object.
    /// @param currentBuffer The buffer being flushed.
    /// @param previousBuffer The buffer on screen, or nullptr when not diffing.
    FlushLinks(TerminalOutput& out, Buffer const& currentBuffer, Buffer const* previousBuffer);

    /// @brief Whether the link covering (@p row, @p col) changed target since the last frame.
    ///
    /// The URI lives beside the cell rather than in it, so Cell equality cannot see it: a link
    /// retargeted while its text stayed byte-identical must still be repainted, or the terminal
    /// keeps opening the old target for the rest of the session.
    [[nodiscard]] bool retargeted(int row, int col) const noexcept
    {
        return any && current.uriAt(row, col) != previous.uriAt(row, col);
    }
};

} // namespace core::tui
