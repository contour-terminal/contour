// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkIndex.hpp>

#include <algorithm>
#include <ranges>

namespace vtbackend
{

void LinkIndex::forget(LinkId id)
{
    auto const found = _links.find(id.value);
    if (found == _links.end())
        return;
    for (auto const& span: found->second.spans)
        if (auto const row = _rows.find(span.row); row != _rows.end())
            std::erase_if(row->second.spans, [id](LinkRowSpan const& s) { return s.link == id; });
    _links.erase(found);
}

void LinkIndex::forgetLinksOn(int64_t row)
{
    auto const entry = _rows.find(row);
    if (entry == _rows.end())
        return;
    auto ids = std::vector<LinkId> {};
    ids.reserve(entry->second.spans.size());
    for (auto const& span: entry->second.spans)
        ids.push_back(span.link);
    for (auto const id: ids)
        forget(id);
}

void LinkIndex::insert(DetectedLink link)
{
    // A link without spans is never reachable, so it is not stored; neither is one with a span on a row that
    // has no entry, since storing it would have to invent that row's revision.
    auto const storable =
        !link.spans.empty() && std::ranges::all_of(link.spans, [this](LinkRowSpan const& span) {
            return _rows.contains(span.row);
        });
    if (!storable)
        return;
    link.id = LinkId(_nextId++);
    for (auto& span: link.spans)
    {
        span.link = link.id;
        auto& spans = _rows.at(span.row).spans;
        spans.insert(std::ranges::upper_bound(spans, span.first, {}, &LinkRowSpan::first), span);
    }
    _links.emplace(link.id.value, std::move(link));
}

DetectedLink const* LinkIndex::linkStartingAt(LinkRowSpan const& span) const noexcept
{
    auto const* const link = find(span.link);
    if (!link || link->spans.empty())
        return nullptr;
    auto const& head = link->spans.front();
    return head.row == span.row && head.first == span.first ? link : nullptr;
}

void LinkIndex::publish(std::span<ScannedRow const> rows, std::vector<DetectedLink> links)
{
    for (auto const& row: rows)
    {
        forgetLinksOn(row.id);
        _rows[row.id].revision = row.revision;
    }
    for (auto& link: links)
        insert(std::move(link));
}

void LinkIndex::add(DetectedLink link, std::span<ScannedRow const> rows)
{
    auto const named = [rows](int64_t row) {
        return std::ranges::any_of(rows, [row](ScannedRow const& candidate) { return candidate.id == row; });
    };
    auto const coversNamedRowsOnly =
        std::ranges::all_of(link.spans, [&named](LinkRowSpan const& span) { return named(span.row); });
    auto const current = std::ranges::all_of(
        rows, [this](ScannedRow const& row) { return scannedRevision(row.id) == row.revision; });
    if (!rows.empty() && coversNamedRowsOnly && current)
        insert(std::move(link));
}

std::optional<uint64_t> LinkIndex::scannedRevision(int64_t row) const noexcept
{
    auto const entry = _rows.find(row);
    return entry != _rows.end() ? std::optional { entry->second.revision } : std::nullopt;
}

DetectedLink const* LinkIndex::at(int64_t row, ColumnOffset column) const noexcept
{
    for (auto const& span: onRow(row))
        if (span.first <= column && column <= span.last)
            return find(span.link);
    return nullptr;
}

std::span<LinkRowSpan const> LinkIndex::onRow(int64_t row) const noexcept
{
    auto const entry = _rows.find(row);
    return entry != _rows.end() ? std::span<LinkRowSpan const> { entry->second.spans }
                                : std::span<LinkRowSpan const> {};
}

DetectedLink const* LinkIndex::find(LinkId id) const noexcept
{
    auto const found = _links.find(id.value);
    return found != _links.end() ? &found->second : nullptr;
}

DetectedLink const* LinkIndex::next(int64_t row, ColumnOffset column) const noexcept
{
    for (auto const& [rowId, entry]: std::ranges::subrange(_rows.lower_bound(row), _rows.end()))
        for (auto const& span: entry.spans)
            if (rowId > row || span.first > column)
                if (auto const* const hit = linkStartingAt(span))
                    return hit;
    return nullptr;
}

DetectedLink const* LinkIndex::previous(int64_t row, ColumnOffset column) const noexcept
{
    for (auto const& [rowId, entry]:
         std::ranges::subrange(_rows.begin(), _rows.upper_bound(row)) | std::views::reverse)
        for (auto const& span: entry.spans | std::views::reverse)
            if (rowId < row || span.first < column)
                if (auto const* const hit = linkStartingAt(span))
                    return hit;
    return nullptr;
}

void LinkIndex::retain(int64_t first, int64_t last)
{
    if (first > last)
    {
        clear();
        return;
    }
    // The rows to keep are [keepFrom, keepEnd); only the two ends outside them are visited.
    auto const keepFrom = _rows.lower_bound(first);
    auto const keepEnd = _rows.upper_bound(last);
    auto doomed = std::vector<int64_t> {};
    for (auto const id: std::ranges::subrange(_rows.begin(), keepFrom) | std::views::keys)
        doomed.push_back(id);
    for (auto const id: std::ranges::subrange(keepEnd, _rows.end()) | std::views::keys)
        doomed.push_back(id);
    for (auto const id: doomed)
        forgetLinksOn(id);
    _rows.erase(_rows.begin(), keepFrom);
    _rows.erase(keepEnd, _rows.end());
}

void LinkIndex::clear() noexcept
{
    _rows.clear();
    _links.clear();
}

} // namespace vtbackend
