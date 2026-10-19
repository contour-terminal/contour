// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <boxed-cpp/boxed.hpp>

namespace vtbackend
{

namespace detail
{
    struct LinkIdTag
    {
    };
} // namespace detail

/// Identifies one detected link while it is indexed; zero is none.
///
/// 64 bits, because ids are allocated for the session's lifetime: a narrower counter would wrap after
/// billions of links, then yield zero and collide with a link still indexed.
using LinkId = boxed::boxed<uint64_t, detail::LinkIdTag>;

/// One row's part of a link.
struct LinkRowSpan
{
    int64_t row = 0;       ///< The row's stable id.
    ColumnOffset first {}; ///< First decorated column.
    ColumnOffset last {};  ///< Last decorated column, inclusive.
    LinkId link {};        ///< The link it belongs to.
};

/// A link found in output.
struct DetectedLink
{
    LinkId id {};         ///< Assigned by the index.
    LinkTarget target {}; ///< What it opens.
    std::string text;     ///< As printed; the tooltip shows the target only where it differs.
    /// One per row it covers, top first. `spans.front()` is the head: the span where the link starts, which
    /// next() and previous() rely on. The index does not sort them.
    std::vector<LinkRowSpan> spans;
};

/// A row as a scan saw it.
struct ScannedRow
{
    int64_t id = 0;        ///< Stable id.
    uint64_t revision = 0; ///< Line::revision() when it was read.
};

/// The detected links of a terminal's scan window, keyed by stable row id.
///
/// Not synchronised: the Terminal that owns it reads and writes it only under the terminal lock, as it does
/// UserMarkIndex.
class LinkIndex
{
  public:
    /// Replaces what is known about @p rows: every link touching one of them goes, @p links come in.
    /// Links with no spans, or with a span on a row not in @p rows, are dropped.
    /// @param rows The rows of one logical line as scanned.
    /// @param links Its links, spans on those rows only and top first (see DetectedLink::spans); their ids
    /// are assigned here.
    void publish(std::span<ScannedRow const> rows, std::vector<DetectedLink> links);

    /// Adds one link found later (a path once its probe confirmed it), if every row in @p rows is still
    /// indexed at the revision given there. Refused when @p rows is empty, when the link has no spans, or
    /// when a span names a row not in @p rows.
    /// @param link The link to add; its spans name the rows, top first (see DetectedLink::spans).
    /// @param rows The rows it was found on, with the revisions it was found at.
    void add(DetectedLink link, std::span<ScannedRow const> rows);

    /// The revision @p row was scanned at.
    /// @param row The row's stable id.
    /// @return The revision, or nullopt when the row was never scanned or was forgotten.
    [[nodiscard]] std::optional<uint64_t> scannedRevision(int64_t row) const noexcept;

    /// The link covering @p column of @p row.
    /// @return The link, or nullptr when none covers that cell.
    [[nodiscard]] DetectedLink const* at(int64_t row, ColumnOffset column) const noexcept;

    /// The spans on @p row, left to right.
    /// @return The spans, empty when the row has none.
    [[nodiscard]] std::span<LinkRowSpan const> onRow(int64_t row) const noexcept;

    /// The link @p id.
    /// @return The link, or nullptr when it is not indexed.
    [[nodiscard]] DetectedLink const* find(LinkId id) const noexcept;

    /// The first link that starts after (@p row, @p column) in reading order.
    /// @return The link, or nullptr when no later link starts.
    [[nodiscard]] DetectedLink const* next(int64_t row, ColumnOffset column) const noexcept;

    /// The last link that starts before (@p row, @p column) in reading order.
    /// @return The link, or nullptr when no earlier link starts.
    [[nodiscard]] DetectedLink const* previous(int64_t row, ColumnOffset column) const noexcept;

    /// Forgets the rows outside [@p first, @p last], and every link touching one of them.
    /// @param first The lowest stable row id to keep.
    /// @param last The highest stable row id to keep; when it is below @p first, nothing is kept.
    void retain(int64_t first, int64_t last);

    /// Forgets everything.
    void clear() noexcept;

    /// Whether no link is indexed.
    /// @return True when the index holds no link.
    [[nodiscard]] bool empty() const noexcept { return _links.empty(); }

    /// How many links are indexed.
    /// @return The count.
    [[nodiscard]] std::size_t size() const noexcept { return _links.size(); }

  private:
    struct RowEntry
    {
        uint64_t revision = 0;
        std::vector<LinkRowSpan> spans;
    };

    void forget(LinkId id);
    void forgetLinksOn(int64_t row);
    void insert(DetectedLink link);
    [[nodiscard]] DetectedLink const* linkStartingAt(LinkRowSpan const& span) const noexcept;

    std::map<int64_t, RowEntry> _rows;
    std::unordered_map<uint64_t, DetectedLink> _links;
    uint64_t _nextId = 1;
};

} // namespace vtbackend
