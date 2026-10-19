// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/links/ScanText.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// Which patterns of the table take part in a scan.
enum class PatternSelection : uint8_t
{
    All = 0, ///< Hint mode: every pattern, and every match, those it may not open included.
    /// Passive detection and click: the clickable patterns, and only the matches that may be opened
    /// (LinkOpening::Allowed).
    ClickableOnly,
};

/// What a scan does with matches that overlap.
enum class LinkOverlap : uint8_t
{
    Keep = 0, ///< Report every match; hint mode resolves first and de-overlaps after.
    /// Earliest start wins, then longest, then a match that is no path, then table order; the others are
    /// dropped.
    Merge,
};

/// Every match of @p patterns in @p text, spanned, trimmed and located as §4.3 says, and mapped to cells.
/// Pure: no lock, no I/O.
/// @param text One logical line.
/// @param patterns The table.
/// @param selection Which of its patterns take part.
/// @param overlap What to do with overlapping matches.
/// @return The candidates. With LinkOverlap::Keep, in pattern order and then position within a pattern
///         (unsorted); with LinkOverlap::Merge, sorted by earliest start, then longest, then a match that
///         is no path, then table order, the overlapped ones dropped.
[[nodiscard]] std::vector<LinkCandidate> findLinks(ScanText const& text,
                                                   LinkPatterns const& patterns,
                                                   PatternSelection selection,
                                                   LinkOverlap overlap);

/// How much of a built-in match is the link: trailing punctuation is dropped, working backwards. A closing
/// bracket goes when unbalanced within the match; any other P* character goes unless it is one of `/&-*_`.
/// @param span The matched text.
/// @return The length, in bytes, that remains.
[[nodiscard]] std::size_t trimmedLength(std::string_view span) noexcept;

} // namespace vtbackend
