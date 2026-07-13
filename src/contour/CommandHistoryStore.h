// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace contour
{

/// Persistence for the command palette's most-recently-used list: the boundary between the in-memory
/// CommandHistory and wherever it actually lives.
///
/// It is an interface per the project's dependency-injection principle (this is filesystem I/O), so
/// tests drive the whole record -> persist -> reload cycle against an in-memory store, and neither
/// the history nor the palette holds any knowledge of files, temp names, or atomic renames.
class CommandHistoryStore
{
  public:
    virtual ~CommandHistoryStore() = default;

    /// Reads back the remembered command ids, newest first.
    ///
    /// @param path Where the store persists (the sibling `command-history.yml` of the loaded config).
    /// @return The ids (empty when nothing has been recorded yet — a first run is not an error), or a
    ///         human-readable error when the file exists but cannot be read.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, std::string> load(
        std::filesystem::path const& path) const = 0;

    /// Replaces the store's entire contents with @p ids, newest first.
    /// @param path Where the store persists.
    /// @param ids  The ids to persist.
    /// @return Nothing on success, or a human-readable error describing why the write failed.
    [[nodiscard]] virtual std::expected<void, std::string> save(std::filesystem::path const& path,
                                                                std::span<std::string const> ids) = 0;
};

/// The production CommandHistoryStore: a YAML file, replaced atomically (write a temp sibling, then
/// rename over the target).
///
/// The atomic dance matters less here than it does for layouts — losing the MRU costs the user some
/// ordering, not their saved work — but a half-written file would fail to PARSE on the next start,
/// and a store that cannot be read is one the user has to go delete by hand. Cheap insurance.
class FileCommandHistoryStore final: public CommandHistoryStore
{
  public:
    [[nodiscard]] std::expected<std::vector<std::string>, std::string> load(
        std::filesystem::path const& path) const override;

    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        std::span<std::string const> ids) override;
};

} // namespace contour
