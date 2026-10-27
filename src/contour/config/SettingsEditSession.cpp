// SPDX-License-Identifier: Apache-2.0
#include <contour/config/SettingsEditSession.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <ranges>
#include <set>
#include <utility>

namespace contour::config
{

namespace
{
    /// "Couldn't save {path}: {reason}."
    [[nodiscard]] SettingsError writeFailed(SideFile const& file, std::string_view reason)
    {
        return SettingsError { .kind = SettingsErrorKind::WriteFailed,
                               .message = std::format("Couldn't save {}: {}.", displayPathOf(file), reason) };
    }

    /// "{path} changed outside Settings; showing that version."
    [[nodiscard]] SettingsError changedOnDisk(SideFile const& file)
    {
        return SettingsError { .kind = SettingsErrorKind::ChangedOnDisk,
                               .message = std::format("{} changed outside Settings; showing that version.",
                                                      displayPathOf(file)) };
    }
} // namespace

SettingsEditSession::SettingsEditSession(GuiConfigStore& store,
                                         SaveScheduler& scheduler,
                                         SettingsEditability editability,
                                         ReloadCallback reload,
                                         ErrorSink reportError):
    _store { store },
    _scheduler { scheduler },
    _editability { editability },
    _reload { std::move(reload) },
    _reportError { std::move(reportError) }
{
}

SettingsEditSession::~SettingsEditSession()
{
    _scheduler.cancel();
}

void SettingsEditSession::observe(SideFile const& file, std::optional<std::string> const& seededFrom)
{
    auto const touched = _touched.find(file);
    if (touched == _touched.end())
    {
        _touched.emplace(file, TouchedFile { .snapshot = seededFrom, .lastSeen = seededFrom });
        return;
    }
    if (_pending.contains(file) || touched->second.lastSeen == seededFrom)
        return;
    touched->second.snapshot = seededFrom;
    touched->second.lastSeen = seededFrom;
}

std::expected<void, SettingsError> SettingsEditSession::touch(SideFile const& file)
{
    if (_touched.contains(file))
        return {};
    auto const disk = readNow(file);
    if (!disk)
        return std::unexpected(disk.error());
    _touched.emplace(file, TouchedFile { .snapshot = *disk, .lastSeen = *disk });
    return {};
}

std::expected<void, SettingsError> SettingsEditSession::stage(SideFile const& file,
                                                              std::optional<std::string> bytes)
{
    auto const one = std::array { StagedFile { .file = file, .bytes = std::move(bytes) } };
    return stageTogether(one);
}

std::expected<void, SettingsError> SettingsEditSession::stageTogether(std::span<StagedFile const> files)
{
    if (_editability == SettingsEditability::Locked)
        return std::unexpected(SettingsError { .kind = SettingsErrorKind::Locked,
                                               .message = std::string { SettingsLockedMessage } });

    for (auto const& staged: files)
        if (auto const touched = touch(staged.file); !touched)
            return touched;
    // Every change this one touches becomes one with it, so that a file staged again keeps the change it was
    // part of -- a rename's new file edited before the flush is still written before the old one goes.
    auto merged = std::set<uint64_t> {};
    for (auto const& staged: files)
        if (auto const pending = _pending.find(staged.file); pending != _pending.end())
            merged.insert(pending->second.change);
    auto const change = merged.empty() ? _nextChange++ : *merged.begin();
    for (auto& [file, pending]: _pending)
        if (merged.contains(pending.change))
            pending.change = change;
    for (auto const& staged: files)
    {
        // A file staged again keeps its place in the order it was first staged in.
        if (auto const pending = _pending.find(staged.file); pending != _pending.end())
            pending->second.bytes = staged.bytes;
        else
            _pending.emplace(staged.file,
                             Pending { .bytes = staged.bytes, .change = change, .order = _nextOrder++ });
    }
    scheduleFlush();
    return {};
}

void SettingsEditSession::scheduleFlush()
{
    _scheduler.schedule(SettingsSaveDebounce, [this] {
        if (auto const flushed = flush(); !flushed)
            _reportError(flushed.error());
    });
}

std::expected<void, SettingsError> SettingsEditSession::flush()
{
    if (_pending.empty())
        return {};
    _scheduler.cancel();
    return settleAll(std::exchange(_pending, {}));
}

std::expected<void, SettingsError> SettingsEditSession::flushChangeOf(SideFile const& file)
{
    auto const staged = _pending.find(file);
    if (staged == _pending.end())
        return {};
    auto const change = staged->second.change;
    auto targets = std::map<SideFile, Pending> {};
    for (auto const& [member, pending]: _pending)
        if (pending.change == change)
            targets.emplace(member, pending);
    std::erase_if(_pending, [change](auto const& entry) { return entry.second.change == change; });
    if (_pending.empty())
        _scheduler.cancel();
    return settleAll(targets);
}

std::expected<void, SettingsError> SettingsEditSession::revert()
{
    _scheduler.cancel();
    _pending.clear();
    // Each file on its own: a file changed on disk keeps its version, and the others still go back.
    auto snapshots = std::map<SideFile, Pending> {};
    for (auto const& [file, touched]: _touched)
        snapshots.emplace(file, Pending { .bytes = touched.snapshot, .change = _nextChange++, .order = 0 });
    return settleAll(snapshots);
}

bool SettingsEditSession::hasChanges() const noexcept
{
    return hasPending() || std::ranges::any_of(_touched, [](auto const& entry) {
               return entry.second.lastSeen != entry.second.snapshot;
           });
}

bool SettingsEditSession::hasPending() const noexcept
{
    return !_pending.empty();
}

PendingChange SettingsEditSession::pendingFor(SideFile const& file) const
{
    auto const found = _pending.find(file);
    if (found == _pending.end())
        return PendingChange::None;
    return found->second.bytes ? PendingChange::Write : PendingChange::Removal;
}

std::expected<std::optional<std::string>, SettingsError> SettingsEditSession::readNow(
    SideFile const& file) const
{
    return _store.read(file).transform_error(
        [&](std::string const& reason) { return writeFailed(file, reason); });
}

std::expected<void, std::string> SettingsEditSession::writeNow(SideFile const& file,
                                                               std::optional<std::string> const& bytes)
{
    return bytes ? _store.write(file, *bytes) : _store.remove(file);
}

SettingsEditSession::Settled SettingsEditSession::settleChange(std::vector<StagedFile> const& files)
{
    auto settled = Settled {};

    // Disk wins: bytes the session has not seen are never written over, and become the new base, so that a
    // later Revert does not write over them either. One such file holds back the whole change, or a rename
    // would leave a copy beside the version written outside Settings.
    auto before = std::vector<std::optional<std::string>> {};
    for (auto const& staged: files)
    {
        auto const disk = readNow(staged.file);
        if (!disk)
        {
            settled.errors.push_back(disk.error());
            return settled;
        }
        auto& touched = _touched.at(staged.file);
        if (*disk != touched.lastSeen)
        {
            touched.snapshot = *disk;
            touched.lastSeen = *disk;
            settled.change = DiskChange::Changed;
            settled.errors.push_back(changedOnDisk(staged.file));
        }
        before.push_back(*disk);
    }
    if (!settled.errors.empty())
        return settled;

    for (auto const index: std::views::iota(size_t { 0 }, files.size()))
    {
        auto const& staged = files[index];
        if (before[index] == staged.bytes)
            continue;
        auto const written = writeNow(staged.file, staged.bytes);
        if (written)
        {
            _touched.at(staged.file).lastSeen = staged.bytes;
            settled.change = DiskChange::Changed;
            continue;
        }
        settled.errors.push_back(writeFailed(staged.file, written.error()));
        // Bytes that reached the disk all the same -- the replace took, a sync did not -- are what the
        // session last saw, or the next flush would take Settings' own bytes for a change made outside it.
        if (auto const after = readNow(staged.file); after && *after == staged.bytes)
        {
            _touched.at(staged.file).lastSeen = staged.bytes;
            settled.change = DiskChange::Changed;
            continue;
        }
        // The change is not done: what it wrote before this file goes back.
        for (auto const undo: std::views::iota(size_t { 0 }, index) | std::views::reverse)
        {
            auto const& done = files[undo];
            if (before[undo] == done.bytes)
                continue;
            if (auto const restored = writeNow(done.file, before[undo]); restored)
                _touched.at(done.file).lastSeen = before[undo];
            else
                settled.errors.push_back(writeFailed(done.file, restored.error()));
        }
        return settled;
    }
    return settled;
}

std::expected<void, SettingsError> SettingsEditSession::settleAll(std::map<SideFile, Pending> const& targets)
{
    // The files of each change, in the order they were staged.
    auto changes = std::map<uint64_t, std::vector<std::pair<uint64_t, StagedFile>>> {};
    for (auto const& [file, pending]: targets)
        changes[pending.change].emplace_back(pending.order,
                                             StagedFile { .file = file, .bytes = pending.bytes });

    auto change = DiskChange::None;
    auto errors = std::vector<SettingsError> {};
    for (auto& [id, members]: changes)
    {
        std::ranges::sort(members, {}, &std::pair<uint64_t, StagedFile>::first);
        auto files = std::vector<StagedFile> {};
        for (auto& member: members)
            files.push_back(std::move(member.second));
        // Every write before any removal: a change that fails part-way then has removed nothing, whatever
        // order its files were staged in (a chained rename keeps its first-staged places).
        std::ranges::stable_partition(files, [](StagedFile const& file) { return file.bytes.has_value(); });
        auto settled = settleChange(files);
        if (settled.change == DiskChange::Changed)
            change = DiskChange::Changed;
        std::ranges::move(settled.errors, std::back_inserter(errors));
    }

    if (change == DiskChange::Changed)
        if (auto const reloaded = _reload(); !reloaded)
            errors.push_back(SettingsError {
                .kind = SettingsErrorKind::ReloadFailed,
                .message = std::format("Couldn't reload the configuration: {}.", reloaded.error()) });

    if (errors.empty())
        return {};
    for (auto const& error: errors | std::views::drop(1))
        _reportError(error);
    return std::unexpected(std::move(errors.front()));
}

} // namespace contour::config
