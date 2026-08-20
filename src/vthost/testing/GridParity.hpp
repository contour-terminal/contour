// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The thin-vs-fat parity oracle: does a daemon-hosted pane hold the same grid the fat GUI
/// would?
///
/// The standard daemon mode has to meet is that a pane behaves and renders identically
/// whether its session lives in this process or in the daemon. That is not a claim anyone
/// can settle by looking — it is a claim about every cell and every line of two grids, so it
/// needs a comparator, and the comparator needs to name WHICH field diverged rather than
/// answering "not equal".
///
/// Both sides are read through @ref vthost::toWireLine — the daemon's own serializer — for two
/// reasons. It is exhaustive over the protocol's vocabulary by construction, so no field can
/// be forgotten here; and comparing what each grid WOULD put on the wire is exactly the
/// question a client populating its grid from the wire has to answer. Rows are expanded to
/// full width first (@ref vthost::expandToFullWidth), so a blank row and a materialized row
/// that look identical are treated as identical — how a row is stored is not a parity
/// difference.
///
/// Hyperlinks are compared by URI, never by id: ids are per-terminal counters, so equal ids
/// would be meaningless and unequal ids a false positive.

#include <vtbackend/screen/Terminal.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vthost::testing
{

/// One field-level divergence between two grids.
struct ParityGap
{
    int64_t line = 0;     ///< Row offset; negative addresses scrollback.
    int column = -1;      ///< Column, or -1 for a whole-line property.
    std::string field;    ///< What differs, e.g. "lineFlags" or "background".
    std::string expected; ///< The authoritative (server) value.
    std::string actual;   ///< What the mirror holds.

    bool operator==(ParityGap const&) const = default;
};

/// Which of a terminal's pages to compare. A terminal is several grids, not one, and a client that
/// mirrors the main page perfectly can still be blank where a status line should be.
enum class ComparedPage : uint8_t
{
    /// The page the user is looking at: the primary screen, or the alternate screen while one is up.
    Active,
    /// The host-writable status line (DECSSDT 2) — its own page with its own grid.
    HostWritableStatus,
};

/// Compares two grids over the rows they both address, field by field.
///
/// @param server The authoritative terminal (the daemon's session).
/// @param mirror The terminal a thin client renders from.
/// @param page Which page of each to compare.
/// @param maxGaps Stop after this many findings — a systemic gap (a line flag nothing
///        applies) otherwise reports once per row and buries everything else.
/// @return Every divergence found, in row-then-column order; empty means parity.
[[nodiscard]] std::vector<ParityGap> compareGrids(vtbackend::Terminal const& server,
                                                  vtbackend::Terminal const& mirror,
                                                  ComparedPage page = ComparedPage::Active,
                                                  std::size_t maxGaps = 40);

/// Renders @p gaps as one line each, for a test failure message.
/// @param gaps What compareGrids returned.
/// @return A human-readable report, empty when @p gaps is.
[[nodiscard]] std::string describeGaps(std::vector<ParityGap> const& gaps);

/// Groups @p gaps by field name and counts them — the shape of the problem rather than its
/// instances, which is what decides where to spend effort.
/// @param gaps What compareGrids returned.
/// @return `field -> row count` lines, most frequent first.
[[nodiscard]] std::string summarizeGaps(std::vector<ParityGap> const& gaps);

} // namespace vthost::testing
