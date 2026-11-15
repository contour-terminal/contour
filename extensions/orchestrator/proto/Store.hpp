// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/proto/StoreKeys.hpp>

namespace orchestrator::proto
{

/// Why the store could not be written.
enum class StoreError : uint8_t
{
    WriteFailed,  ///< The value's file could not be written in full.
    SyncFailed,   ///< The value's file, or the directory after a rename or removal, could not be synced.
    RenameFailed, ///< The written file could not replace the old one.
    RemoveFailed, ///< The value's file could not be removed.
};

/// The extension's store: one file per key in one directory, each written and synced, renamed into place, and
/// the directory synced, before the next effect is carried out.
class FileStore
{
  public:
    /// @param directory The directory the files live in; created on the first write.
    explicit FileStore(std::string directory): _directory { std::move(directory) } {}

    /// @return Every stored entry.
    [[nodiscard]] std::vector<StoredEntry> load() const;
    /// Stores @p value under @p key, durably.
    [[nodiscard]] std::expected<void, StoreError> set(std::string_view key, std::string_view value) const;
    /// Removes @p key; a key never stored is removed already.
    [[nodiscard]] std::expected<void, StoreError> remove(std::string_view key) const;
    /// @return The directory the files live in.
    [[nodiscard]] std::string const& directory() const noexcept { return _directory; }

  private:
    std::string _directory;
};

} // namespace orchestrator::proto
