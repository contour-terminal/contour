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

    /// Whether a new prompt or the process's exit ends @p record: what it reports is transient, or an
    /// integration, which speaks for a run of an agent that the prompt or the exit has just ended.
    [[nodiscard]] bool endsWithPrompt(ProgramStatusRecord const& record) noexcept
    {
        return isTransient(record) || record.origin == ProgramStatusOrigin::Integration;
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

ProgramStatusChange ProgramStatusSet::apply(ProgramStatusRecord record,
                                            std::chrono::system_clock::time_point now)
{
    auto change = ProgramStatusChange { .updated = { record.id },
                                        .removed = {},
                                        .reason = ProgramStatusReason::Report };
    stamp(record, now);
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

ProgramStatusChange ProgramStatusSet::clearIntegration(string_view id)
{
    return removeIf(
        [id](ProgramStatusRecord const& record) {
            return record.origin == ProgramStatusOrigin::Integration && record.id == id;
        },
        ProgramStatusReason::Report);
}

ProgramStatusChange ProgramStatusSet::promptStarted()
{
    return removeIf(endsWithPrompt, ProgramStatusReason::Prompt);
}

ProgramStatusChange ProgramStatusSet::processExited()
{
    return removeIf(endsWithPrompt, ProgramStatusReason::ProcessExit);
}

ProgramStatusChange ProgramStatusSet::noteUserInput(std::chrono::system_clock::time_point now)
{
    auto change =
        ProgramStatusChange { .updated = {}, .removed = {}, .reason = ProgramStatusReason::UserInput };
    for (auto& record: _records)
    {
        if (record.origin != ProgramStatusOrigin::Integration || record.state != ProgramState::Blocked)
            continue;
        record.state = ProgramState::Working;
        record.kind = BlockedKind::None;
        record.since = now;
        record.generation = _nextGeneration++;
        change.updated.push_back(record.id);
    }
    if (change.updated.empty())
        return change;
    // Updated is updated: the unblocked records move to the end, as apply() moves the one it stores, so that
    // the set stays least recently updated first.
    std::ranges::stable_partition(_records, [&](ProgramStatusRecord const& record) {
        return std::ranges::find(change.updated, record.id) == change.updated.end();
    });
    return change;
}

void ProgramStatusSet::stamp(ProgramStatusRecord& record, std::chrono::system_clock::time_point now)
{
    if (auto const* const before = find(record.id);
        before && before->state == record.state && before->kind == record.kind)
    {
        record.since = before->since;
        record.generation = before->generation;
    }
    else
    {
        record.since = now;
        record.generation = _nextGeneration++;
    }
}

ProgramStatusChange ProgramStatusSet::reset()
{
    return removeIf([](ProgramStatusRecord const&) { return true; }, ProgramStatusReason::Reset);
}

ProgramStatusChange ProgramStatusSet::replaceAll(std::vector<ProgramStatusRecord> records,
                                                 ProgramStatusTiming timing,
                                                 std::chrono::system_clock::time_point now)
{
    if (records.size() > _maxRecords)
        records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(_maxRecords));

    // Before the comparison below, which would otherwise see every unstamped record as changed.
    if (timing == ProgramStatusTiming::Absent)
        for (auto& record: records)
            stamp(record, now);

    auto change = ProgramStatusChange { .updated = {}, .removed = {}, .reason = ProgramStatusReason::Mirror };
    for (auto const& record: records)
        if (auto const* const before = find(record.id); !before || *before != record)
            change.updated.push_back(record.id);
    for (auto const& record: _records)
        if (std::ranges::find(records, record.id, &ProgramStatusRecord::id) == records.end())
            change.removed.push_back(record.id);
    for (auto const& record: records)
        _nextGeneration = std::max(_nextGeneration, record.generation + 1);
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
