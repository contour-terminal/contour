// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtconformance
{

/// Finds marker strings in a byte stream that arrives in arbitrary chunks.
///
/// The barrier a conformance driver needs is "the program under test is about to block for me". The
/// byte stream is the only channel that answers that honestly and in time: vttest flushes stdout
/// before every blocking read (`tprintf`/`cprintf` end in `FLUSH`, esc.c:7,371,394) but never flushes
/// its log (setup.c:359 opens it with no `setvbuf`), and `readnl()` -- the read behind every "Push
/// <RETURN>" -- writes nothing at all before parking. So the banner on the wire *is* the fact that it
/// is about to block. Keying on that is causal; waiting for output to fall quiet is a guess.
///
/// A marker can straddle two reads, so the scanner remembers the tail of what it has already seen and
/// searches across the seam. It is a pure function of the bytes: no clock, no screen, no PTY, and no
/// knowledge of what any marker means.
class MarkerScanner
{
  public:
    /// Where a marker finished, and which one it was.
    struct Match
    {
        /// One past the marker's last byte, in the current chunk's coordinates.
        ///
        /// This is the cut: everything up to here belongs to the screen the marker announces, and
        /// everything after it belongs to whatever the program does next.
        size_t endOffset;

        /// Index into the marker table the scanner was built with.
        size_t markerIndex;
    };

    /// @param markers The byte sequences to look for. Borrowed; must outlive the scanner. Empty
    ///                markers are ignored -- one would match everywhere and mean nothing.
    explicit MarkerScanner(std::span<std::string_view const> markers);

    /// Scans @p chunk, picking up any marker that began in an earlier one.
    ///
    /// @return Every marker that *finishes* inside @p chunk, ordered by where it ends and then by
    ///         table order. A marker already reported is never reported twice.
    [[nodiscard]] std::vector<Match> scan(std::string_view chunk);

    /// @return The bytes retained to catch a marker straddling the next seam. @see scan.
    [[nodiscard]] std::string_view tail() const noexcept { return _tail; }

  private:
    std::span<std::string_view const> _markers;

    /// The last `_longest - 1` bytes scanned.
    ///
    /// Exactly enough, and no more: a marker of length L can have at most L-1 of its bytes before a
    /// seam, so keeping fewer would miss one that started just outside the window. It is a scanning
    /// artifact only -- bytes are fed to the terminal the moment they arrive, and nothing is held back.
    std::string _tail;

    /// The longest marker's length, which sets the tail size.
    size_t _longest = 0;
};

} // namespace vtconformance
