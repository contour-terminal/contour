// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Store.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <orchestrator/proto/Host.hpp>
#include <orchestrator/proto/Spawn.hpp>

namespace orchestrator::proto
{

namespace fs = std::filesystem;

namespace
{
    /// The name a value is written under before it replaces the old file: no key escapes to it.
    constexpr auto TemporaryName = std::string_view { "%tmp" };

    /// Writes @p value to @p path in full and syncs it, before it is renamed into place.
    [[nodiscard]] std::expected<void, StoreError> writeSynced(std::string const& path, std::string_view value)
    {
        auto const file = Descriptor { ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) };
        if (file.get() < 0)
            return std::unexpected { StoreError::WriteFailed };
        while (!value.empty())
        {
            auto const written = ::write(file.get(), value.data(), value.size());
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return std::unexpected { StoreError::WriteFailed };
            value.remove_prefix(static_cast<std::size_t>(written));
        }
        if (::fsync(file.get()) != 0)
            return std::unexpected { StoreError::SyncFailed };
        return {};
    }

    /// Syncs @p directory, so that a rename or removal in it is durable.
    [[nodiscard]] std::expected<void, StoreError> syncDirectory(std::string const& directory)
    {
        auto const handle = Descriptor { ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
        if (handle.get() < 0 || ::fsync(handle.get()) != 0)
            return std::unexpected { StoreError::SyncFailed };
        return {};
    }
} // namespace

std::vector<StoredEntry> FileStore::load() const
{
    auto entries = std::vector<StoredEntry> {};
    auto error = std::error_code {};
    for (auto const& file: fs::directory_iterator(_directory, error))
    {
        auto key = unescapeKey(file.path().filename().string());
        if (!key || !file.is_regular_file(error))
            continue;
        if (auto value = readWholeFile(file.path().string()))
            entries.push_back(StoredEntry { .key = std::move(*key), .value = std::move(*value) });
    }
    return entries;
}

std::expected<void, StoreError> FileStore::set(std::string_view key, std::string_view value) const
{
    auto error = std::error_code {};
    fs::create_directories(_directory, error);
    auto const temporary = std::format("{}/{}", _directory, TemporaryName);
    if (auto const written = writeSynced(temporary, value); !written)
        return written;
    if (std::rename(temporary.c_str(), std::format("{}/{}", _directory, escapeKey(key)).c_str()) != 0)
        return std::unexpected { StoreError::RenameFailed };
    return syncDirectory(_directory);
}

std::expected<void, StoreError> FileStore::remove(std::string_view key) const
{
    auto error = std::error_code {};
    fs::remove(std::format("{}/{}", _directory, escapeKey(key)), error);
    if (error)
        return std::unexpected { StoreError::RemoveFailed };
    return syncDirectory(_directory);
}

} // namespace orchestrator::proto
