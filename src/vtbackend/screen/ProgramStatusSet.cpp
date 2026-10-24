// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/ProgramStatusSet.hpp>

#include <algorithm>
#include <utility>

using std::string_view;

namespace vtbackend
{

namespace
{
    /// Whether @p id is @p root or lies beneath it; every id lies beneath the empty root.
    [[nodiscard]] bool isWithin(string_view id, string_view root) noexcept
    {
        if (root.empty() || id == root)
            return true;
        return id.size() > root.size() && id.starts_with(root) && id[root.size()] == '/';
    }

    /// Whether @p record lives only as long as what it reports runs.
    [[nodiscard]] bool isTransient(ProgramStatusRecord const& record) noexcept
    {
        return record.state == ProgramState::Working || record.state == ProgramState::Blocked;
    }

    /// The parent of @p id: its path less the last segment, or the root.
    [[nodiscard]] string_view parentOf(string_view id) noexcept
    {
        auto const slash = id.rfind('/');
        return slash == string_view::npos ? string_view {} : id.substr(0, slash);
    }
} // namespace

ProgramStatusSet::ProgramStatusSet(std::size_t maxRecords):
    _maxRecords { std::max<std::size_t>(maxRecords, 1) }
{
}

ProgramStatusRecord const* ProgramStatusSet::find(string_view id) const noexcept
{
    auto const found = std::ranges::find(_records, id, &ProgramStatusRecord::id);
    return found == _records.end() ? nullptr : &*found;
}

ProgramStatusChange ProgramStatusSet::apply(ProgramStatusRecord record)
{
    auto change = ProgramStatusChange { .updated = { record.id },
                                        .removed = {},
                                        .reason = ProgramStatusReason::Report };
    std::erase_if(_records, [&](ProgramStatusRecord const& existing) { return existing.id == record.id; });
    if (_records.size() >= _maxRecords)
    {
        change.removed.push_back(std::move(_records.front().id));
        _records.erase(_records.begin());
    }
    _records.push_back(std::move(record));
    return change;
}

template <typename Predicate>
ProgramStatusChange ProgramStatusSet::removeIf(Predicate drops, ProgramStatusReason reason)
{
    auto change = ProgramStatusChange { .updated = {}, .removed = {}, .reason = reason };
    for (auto const& record: _records)
        if (drops(record))
            change.removed.push_back(record.id);
    std::erase_if(_records, drops);
    return change;
}

ProgramStatusChange ProgramStatusSet::clear(string_view id)
{
    return removeIf([id](ProgramStatusRecord const& record) { return isWithin(record.id, id); },
                    ProgramStatusReason::Report);
}

ProgramStatusChange ProgramStatusSet::promptStarted()
{
    return removeIf(isTransient, ProgramStatusReason::Prompt);
}

ProgramStatusChange ProgramStatusSet::processExited()
{
    return removeIf(isTransient, ProgramStatusReason::ProcessExit);
}

ProgramStatusChange ProgramStatusSet::reset()
{
    return removeIf([](ProgramStatusRecord const&) { return true; }, ProgramStatusReason::Reset);
}

ProgramStatusChange ProgramStatusSet::replaceAll(std::vector<ProgramStatusRecord> records)
{
    if (records.size() > _maxRecords)
        records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(_maxRecords));

    auto change = ProgramStatusChange { .updated = {}, .removed = {}, .reason = ProgramStatusReason::Mirror };
    for (auto const& record: records)
        if (auto const* const before = find(record.id); !before || *before != record)
            change.updated.push_back(record.id);
    for (auto const& record: _records)
        if (std::ranges::find(records, record.id, &ProgramStatusRecord::id) == records.end())
            change.removed.push_back(record.id);
    _records = std::move(records);
    return change;
}

string_view ProgramStatusSet::appOf(ProgramStatusRecord const& record) const noexcept
{
    if (!record.app.empty())
        return record.app;
    auto id = string_view { record.id };
    while (!id.empty())
    {
        id = parentOf(id);
        if (auto const* const ancestor = find(id); ancestor && !ancestor->app.empty())
            return ancestor->app;
    }
    return {};
}

} // namespace vtbackend
