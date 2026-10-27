// SPDX-License-Identifier: Apache-2.0
#include <contour/config/SettingsEditSession.hpp>

#include <algorithm>
#include <format>
#include <ranges>
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

std::expected<void, SettingsError> SettingsEditSession::stage(SideFile const& file,
                                                              std::optional<std::string> bytes)
{
    if (_editability == SettingsEditability::Locked)
        return std::unexpected(SettingsError { .kind = SettingsErrorKind::Locked,
                                               .message = std::string { SettingsLockedMessage } });

    if (!_touched.contains(file))
    {
        auto const disk = readNow(file);
        if (!disk)
            return std::unexpected(disk.error());
        _touched.emplace(file, TouchedFile { .snapshot = *disk, .lastSeen = *disk });
    }
    _pending.insert_or_assign(file, std::move(bytes));
    _scheduler.schedule(SettingsSaveDebounce, [this] {
        if (auto const flushed = flush(); !flushed)
            _reportError(flushed.error());
    });
    return {};
}

std::expected<void, SettingsError> SettingsEditSession::flush()
{
    if (_pending.empty())
        return {};
    _scheduler.cancel();
    return settleAll(std::exchange(_pending, {}));
}

std::expected<void, SettingsError> SettingsEditSession::revert()
{
    _scheduler.cancel();
    _pending.clear();
    auto snapshots = std::map<SideFile, std::optional<std::string>> {};
    for (auto const& [file, touched]: _touched)
        snapshots.emplace(file, touched.snapshot);
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

bool SettingsEditSession::hasPendingFor(SideFile const& file) const
{
    return _pending.contains(file);
}

std::expected<std::optional<std::string>, SettingsError> SettingsEditSession::readNow(
    SideFile const& file) const
{
    return _store.read(file).transform_error(
        [&](std::string const& reason) { return writeFailed(file, reason); });
}

SettingsEditSession::Settled SettingsEditSession::settle(SideFile const& file,
                                                         std::optional<std::string> const& bytes)
{
    auto& touched = _touched.at(file);
    auto const disk = readNow(file);
    if (!disk)
        return Settled { .change = DiskChange::None, .error = disk.error() };

    // Disk wins: bytes the session has not seen are never written over, and become the new base, so that
    // a later Revert does not write over them either.
    if (*disk != touched.lastSeen)
    {
        touched.snapshot = *disk;
        touched.lastSeen = *disk;
        return Settled { .change = DiskChange::Changed, .error = changedOnDisk(file) };
    }
    if (*disk == bytes)
        return Settled {};

    auto const written = bytes ? _store.write(file, *bytes) : _store.remove(file);
    if (!written)
        return Settled { .change = DiskChange::None, .error = writeFailed(file, written.error()) };
    touched.lastSeen = bytes;
    return Settled { .change = DiskChange::Changed, .error = std::nullopt };
}

std::expected<void, SettingsError> SettingsEditSession::settleAll(
    std::map<SideFile, std::optional<std::string>> const& targets)
{
    auto change = DiskChange::None;
    auto errors = std::vector<SettingsError> {};
    for (auto const& [file, bytes]: targets)
    {
        auto settled = settle(file, bytes);
        if (settled.change == DiskChange::Changed)
            change = DiskChange::Changed;
        if (settled.error)
            errors.push_back(std::move(*settled.error));
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
