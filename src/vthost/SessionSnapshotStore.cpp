// SPDX-License-Identifier: Apache-2.0
#include <vthost/SessionSnapshotStore.hpp>

#include <core/Utf8Path.hpp>

#include <format>
#include <span>
#include <tuple>
#include <utility>

#include <vthost/SessionSnapshotCodec.hpp>

namespace vthost
{

namespace
{
    [[nodiscard]] StoreFailure failure(StoreStep step, std::string detail)
    {
        return StoreFailure { .step = step, .detail = std::move(detail) };
    }

    template <typename T>
    [[nodiscard]] std::expected<T, StoreFailure> failedAt(StoreStep step,
                                                          std::expected<T, std::string> result)
    {
        return std::move(result).transform_error(
            [step](std::string detail) { return failure(step, std::move(detail)); });
    }

    /// Removes @p path, or leaves it: every caller is already on a path where there is nothing to do about
    /// it.
    void removeBestEffort(core::platform::FileSystem const& fileSystem, std::filesystem::path const& path)
    {
        std::ignore = fileSystem.remove(path);
    }

    [[nodiscard]] std::span<std::byte const> bytesOf(std::string const& text) noexcept
    {
        return std::as_bytes(std::span { text.data(), text.size() });
    }
} // namespace

SessionSnapshotStore::SessionSnapshotStore(core::platform::FileSystem const& fileSystem,
                                           IFilePrivacy const& privacy,
                                           std::filesystem::path path):
    _fileSystem { fileSystem }, _privacy { privacy }, _path { std::move(path) }
{
}

std::filesystem::path SessionSnapshotStore::sibling(std::string_view suffix) const
{
    auto name = _path.filename().string();
    name += suffix;
    return _path.parent_path() / name;
}

std::expected<void, StoreFailure> SessionSnapshotStore::write(WorkspaceSnapshot const& snapshot) const
{
    auto const bytes = encodeSnapshot(snapshot);
    if (bytes.size() > MaxSnapshotFileBytes)
        return std::unexpected(failure(
            StoreStep::WriteTemporary,
            std::format("the snapshot ({} bytes) exceeds {} bytes", bytes.size(), MaxSnapshotFileBytes)));

    auto const directory = _path.parent_path();
    auto const temporary = sibling(".tmp");
    auto const restrictTo = [&](std::filesystem::path const& target, PrivateEntry kind) {
        return _privacy.restrictToOwner(target, kind).transform_error([](std::string detail) {
            return failure(StoreStep::Restrict, std::move(detail));
        });
    };
    auto const content = std::string_view { reinterpret_cast<char const*>(bytes.data()), bytes.size() };

    if (auto const created = failedAt(StoreStep::CreateDirectory, _fileSystem.createDirectories(directory));
        !created)
        return std::unexpected(created.error());
    if (auto const restricted = restrictTo(directory, PrivateEntry::Directory); !restricted)
        return restricted;

    // Created exclusively, so a link planted where it goes is not followed, after dropping what a crash
    // left there. It is restricted while still empty: a rewrite and a rename both keep a file's mode and
    // DACL, so the content never exists under anyone else's reach. It is synced before the rename, or the
    // rename could reach the disk before the content it names does.
    removeBestEffort(_fileSystem, temporary);
    auto const step = [&]() -> std::expected<void, StoreFailure> {
        return failedAt(StoreStep::WriteTemporary, _fileSystem.createNewFile(temporary))
            .and_then([&] { return restrictTo(temporary, PrivateEntry::File); })
            .and_then([&] {
                return failedAt(StoreStep::WriteTemporary, _fileSystem.writeFile(temporary, content));
            })
            .and_then([&] { return failedAt(StoreStep::SyncTemporary, _fileSystem.syncFile(temporary)); })
            .and_then([&] { return failedAt(StoreStep::Replace, _fileSystem.replaceFile(temporary, _path)); })
            .and_then(
                [&] { return failedAt(StoreStep::SyncDirectory, _fileSystem.syncDirectory(directory)); });
    }();
    if (!step)
        removeBestEffort(_fileSystem, temporary);
    return step;
}

std::expected<void, StoreFailure> SessionSnapshotStore::clear() const
{
    return write(WorkspaceSnapshot {});
}

std::expected<void, StoreFailure> SessionSnapshotStore::discard() const
{
    return clear().and_then([this] { return confirmRestored(); });
}

LoadResult SessionSnapshotStore::loadFile(std::filesystem::path const& file,
                                          RestoreSource source,
                                          DamagedSnapshot damaged) const
{
    auto result = LoadResult { .source = source };
    if (!_fileSystem.exists(file))
        return result;

    auto const setAside = [&](std::string detail) {
        if (damaged == DamagedSnapshot::LeftInPlace)
        {
            result.verdict = LoadVerdict::Unreadable;
            result.detail = std::move(detail);
            return result;
        }
        auto const aside = std::filesystem::path { file.string() + ".corrupt" };
        removeBestEffort(_fileSystem, aside);
        if (!_fileSystem.rename(file, aside))
            removeBestEffort(_fileSystem, file);
        result.verdict = LoadVerdict::SetAside;
        result.detail = std::move(detail);
        return result;
    };

    auto const size = _fileSystem.fileSize(file);
    if (!size)
    {
        result.verdict = LoadVerdict::Unreadable;
        result.detail = size.error();
        return result;
    }
    if (*size > MaxSnapshotFileBytes)
        return setAside(std::string { describe(SnapshotError::TooLarge) });

    auto const content = _fileSystem.readFile(file);
    if (!content)
    {
        result.verdict = LoadVerdict::Unreadable;
        result.detail = content.error();
        return result;
    }
    auto decoded = decodeSnapshot(bytesOf(*content));
    if (!decoded)
    {
        if (decoded.error() == SnapshotError::UnsupportedFormat)
        {
            result.verdict = LoadVerdict::Unsupported;
            result.detail = std::string { describe(decoded.error()) };
            return result;
        }
        return setAside(std::string { describe(decoded.error()) });
    }
    result.droppedHistories = decoded->droppedHistories;
    if (decoded->snapshot.empty())
    {
        result.verdict = LoadVerdict::Empty;
        return result;
    }
    result.verdict = LoadVerdict::Restorable;
    result.snapshot = std::move(decoded->snapshot);
    return result;
}

LoadResult SessionSnapshotStore::load() const
{
    auto pending = loadFile(sibling(".restored"), RestoreSource::PendingRestore, DamagedSnapshot::SetAside);
    if (pending.verdict == LoadVerdict::Absent)
        return loadFile(_path, RestoreSource::Snapshot, DamagedSnapshot::SetAside);
    if (pending.verdict != LoadVerdict::SetAside)
        return pending;
    // The retry copy is damaged, which loses nothing if the file the daemon has been writing since is not.
    auto current = loadFile(_path, RestoreSource::Snapshot, DamagedSnapshot::SetAside);
    return current.verdict == LoadVerdict::Absent ? pending : current;
}

LoadResult SessionSnapshotStore::peek() const
{
    // The newer of the two: what a crashed daemon left is the workspace it last wrote, and a `.restored` copy
    // beside it is older than that, kept for a restore's retry (load()), not for what was on screen.
    auto pending =
        loadFile(sibling(".restored"), RestoreSource::PendingRestore, DamagedSnapshot::LeftInPlace);
    auto current = loadFile(_path, RestoreSource::Snapshot, DamagedSnapshot::LeftInPlace);
    if (pending.verdict != LoadVerdict::Restorable)
        return current;
    if (current.verdict != LoadVerdict::Restorable)
        return pending;
    return current.snapshot->takenAt >= pending.snapshot->takenAt ? std::move(current) : std::move(pending);
}

std::expected<void, StoreFailure> SessionSnapshotStore::beginRestore(RestoreSource source) const
{
    if (source == RestoreSource::PendingRestore)
        return {};
    return failedAt(StoreStep::Replace, _fileSystem.rename(_path, sibling(".restored")));
}

std::expected<void, StoreFailure> SessionSnapshotStore::endRestore(RestoreSource source,
                                                                   RestoreOutcome outcome) const
{
    if (outcome == RestoreOutcome::Realized || source == RestoreSource::Snapshot)
        return {};
    auto const failed = sibling(".failed");
    removeBestEffort(_fileSystem, failed);
    return failedAt(StoreStep::Replace, _fileSystem.rename(sibling(".restored"), failed));
}

std::expected<void, StoreFailure> SessionSnapshotStore::confirmRestored() const
{
    return failedAt(StoreStep::Replace, _fileSystem.remove(sibling(".restored"))).transform([](bool) {});
}

bool isFileNameLabel(std::string_view label)
{
    constexpr auto Refused = std::string_view { "/\\:\0", 4 };
    return !label.empty() && label.find_first_of(Refused) == std::string_view::npos && !label.contains("..");
}

std::optional<std::filesystem::path> sessionSnapshotPath(std::filesystem::path const& stateDirectory,
                                                         std::string_view label)
{
    if (!isFileNameLabel(label))
        return std::nullopt;
    return stateDirectory / "sessions" / std::format("{}.snapshot", label);
}

std::optional<std::filesystem::path> daemonSnapshotPath(std::filesystem::path const& stateDirectory,
                                                        std::string_view label,
                                                        std::filesystem::path const& socket,
                                                        std::filesystem::path const& labelSocket)
{
    auto const normal = socket.lexically_normal();
    if (normal == labelSocket.lexically_normal())
        return sessionSnapshotPath(stateDirectory, label);
    auto const text = core::pathToUtf8(normal);
    auto const keyed = std::format("{}@{:016x}", label, fnv1a64(std::as_bytes(std::span { text })));
    return sessionSnapshotPath(stateDirectory, keyed);
}

} // namespace vthost
