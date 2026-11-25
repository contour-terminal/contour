// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ProcessRunner.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vtpty
{

/// Keeps what a process writes to one output stream, as it is read: a line that begins with one of the
/// dropped prefixes is discarded whole, line end included, and of what remains at most a cap is kept. The
/// cap so bounds what the process wrote that matters, not everything it wrote: a listing of a million lines
/// of which the reader wants three fits in any cap once the rest are dropped.
///
/// Only a line's first bytes are held back while they could still be a dropped prefix, so the memory it
/// takes beyond the cap is at most the longest prefix.
class StreamKeeper
{
  public:
    /// @param cap How many bytes are kept; 0 to keep none, so that anything kept marks it Truncated.
    /// @param droppedPrefixes A line that begins with one of these is dropped; none is empty. Empty: every
    ///        byte counts against the cap.
    StreamKeeper(std::size_t cap, std::vector<std::string> droppedPrefixes);

    /// Takes the next bytes the stream gave.
    /// @param bytes What one read returned.
    void take(std::string_view bytes);

    /// @return What was kept, the stream having ended: a last line without a line end is judged as one with.
    [[nodiscard]] CapturedStream finish() &&;

  private:
    /// Where the line being read stands.
    enum class LineState : uint8_t
    {
        Deciding, ///< Its first bytes could still be a dropped prefix: they are held in _lineStart.
        Keeping,  ///< It begins with no dropped prefix: its bytes count against the cap.
        Dropping, ///< It begins with a dropped prefix: its bytes are discarded up to its line end.
    };

    /// Keeps @p bytes up to the cap; what does not fit marks the stream Truncated.
    void keep(std::string_view bytes);

    /// Moves a Deciding line on once _lineStart is a dropped prefix, or the start of none.
    void decide();

    std::size_t _cap;
    std::vector<std::string> _droppedPrefixes;
    LineState _state;
    std::string _lineStart; ///< The line's bytes so far, while it is Deciding.
    CapturedStream _captured;
};

} // namespace vtpty
