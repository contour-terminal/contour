// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The registry of the agent panes each daemon-scoped extension opened: one file per extension, beside its
/// store, one line per pane (`<pane-uuid> <tag>`), written whole and durably. It is what tells a restarted
/// daemon which of its panes are an extension's -- and, read before the sessions are restored, why nothing
/// may be concluded from it until they are (@see OwnedPanes::reconcile).

#include <core/platform/FileSystem.hpp>

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtcontrol/Uuid.hpp>

namespace vthost
{

/// One line of the registry file: a pane an extension opened.
struct OwnedPaneRecord
{
    vtcontrol::Uuid pane; ///< The pane's UUID.
    std::string tag;      ///< The extension's own label for it.

    bool operator==(OwnedPaneRecord const&) const = default;
};

/// Why a registry file was not written.
enum class OwnedPaneStoreError : uint8_t
{
    Unnamed,     ///< The extension has no registry file: its name cannot name one.
    WriteFailed, ///< The file could not be written whole and durably.
};

/// Where the registry files are kept.
class IOwnedPaneStore
{
  public:
    IOwnedPaneStore() = default;
    IOwnedPaneStore(IOwnedPaneStore const&) = delete;
    IOwnedPaneStore(IOwnedPaneStore&&) = delete;
    IOwnedPaneStore& operator=(IOwnedPaneStore const&) = delete;
    IOwnedPaneStore& operator=(IOwnedPaneStore&&) = delete;
    virtual ~IOwnedPaneStore() = default;

    /// @return Every extension's records, as their files hold them; an unreadable line or file is skipped.
    [[nodiscard]] virtual std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> loadAll()
        const = 0;

    /// Replaces @p extension's file with @p records, whole and durably.
    /// @param extension What the configuration calls it.
    /// @param records Its panes, in order.
    /// @return Nothing once durable; or why it is not.
    [[nodiscard]] virtual std::expected<void, OwnedPaneStoreError> save(
        std::string_view extension, std::span<OwnedPaneRecord const> records) = 0;
};

/// @param text A registry file's content.
/// @return Its records; a line that is not `<uuid> <tag>` is skipped.
[[nodiscard]] std::vector<OwnedPaneRecord> parseOwnedPanes(std::string_view text);

/// @param records Some records.
/// @return The registry file that holds them.
[[nodiscard]] std::string formatOwnedPanes(std::span<OwnedPaneRecord const> records);

/// The registry files over a filesystem, one per extension it was told of.
class FileOwnedPaneStore final: public IOwnedPaneStore
{
  public:
    /// @param fileSystem Where the files are; outlives the store.
    /// @param files Each extension's registry file (@see daemonExtensionPanesFile).
    FileOwnedPaneStore(core::platform::FileSystem const& fileSystem,
                       std::map<std::string, std::filesystem::path, std::less<>> files);

    [[nodiscard]] std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> loadAll() const override;
    [[nodiscard]] std::expected<void, OwnedPaneStoreError> save(
        std::string_view extension, std::span<OwnedPaneRecord const> records) override;

  private:
    core::platform::FileSystem const& _fileSystem;
    std::map<std::string, std::filesystem::path, std::less<>> _files;
};

/// Writes another store's files off the caller's thread: save() hands the records to a writer thread and
/// returns at once, the newest records of an extension replacing those still waiting, so that the host's
/// event loop never waits for a disk. The thread is started by the first save(); destruction writes what
/// waits, then joins it. loadAll() reads the store itself, at once.
class AsyncOwnedPaneStore final: public IOwnedPaneStore
{
  public:
    /// @param store What is written to; outlives this.
    explicit AsyncOwnedPaneStore(IOwnedPaneStore& store);
    AsyncOwnedPaneStore(AsyncOwnedPaneStore const&) = delete;
    AsyncOwnedPaneStore(AsyncOwnedPaneStore&&) = delete;
    AsyncOwnedPaneStore& operator=(AsyncOwnedPaneStore const&) = delete;
    AsyncOwnedPaneStore& operator=(AsyncOwnedPaneStore&&) = delete;
    /// Writes what waits, then stops the thread.
    ~AsyncOwnedPaneStore() override;

    [[nodiscard]] std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> loadAll() const override;

    /// Queues @p records as @p extension's file; never waits for the disk.
    /// @return Nothing: a write that fails is the writer's to log.
    [[nodiscard]] std::expected<void, OwnedPaneStoreError> save(
        std::string_view extension, std::span<OwnedPaneRecord const> records) override;

    /// Waits until everything saved so far is written.
    void flush();

  private:
    void run(std::stop_token const& stop);

    IOwnedPaneStore& _store;
    std::mutex _mutex; ///< Guards everything below but the thread.
    std::condition_variable_any _changed;
    std::map<std::string, std::vector<OwnedPaneRecord>, std::less<>> _pending;
    uint64_t _submitted = 0; ///< How many save() calls were queued.
    uint64_t _written = 0;   ///< How many of them reached the store (or were replaced before).
    std::jthread _thread;    ///< Last: started on the first save, it reaches the members above.
};

/// The registry of a host that keeps none across restarts: what it is told it holds, in memory.
class InMemoryOwnedPaneStore final: public IOwnedPaneStore
{
  public:
    InMemoryOwnedPaneStore() = default;

    /// @param records What it holds from the start, per extension.
    explicit InMemoryOwnedPaneStore(std::map<std::string, std::vector<OwnedPaneRecord>, std::less<>> records):
        _records { std::move(records) }
    {
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> loadAll() const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return { _records.begin(), _records.end() };
    }

    /// Saved from AsyncOwnedPaneStore's writer thread, read from any other.
    [[nodiscard]] std::expected<void, OwnedPaneStoreError> save(
        std::string_view extension, std::span<OwnedPaneRecord const> records) override
    {
        auto const lock = std::scoped_lock { _mutex };
        _records.insert_or_assign(std::string { extension },
                                  std::vector<OwnedPaneRecord>(records.begin(), records.end()));
        return {};
    }

  private:
    mutable std::mutex _mutex;
    std::map<std::string, std::vector<OwnedPaneRecord>, std::less<>> _records;
};

} // namespace vthost
