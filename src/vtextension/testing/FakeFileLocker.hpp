// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <set>
#include <utility>

#include <vtextension/FileLocker.hpp>

namespace vtextension::testing
{

/// What a FakeFileLocker's locks do.
enum class FakeLocking : uint8_t
{
    Works,       ///< A lock is taken unless held.
    FailsWithIo, ///< Every lock answers `Io`, as a lock file that cannot be created does.
};

/// Exclusive locks kept in memory: a path is held while a lock on it lives.
class FakeFileLocker final: public IFileLocker
{
  public:
    /// The paths whose lock is held now.
    [[nodiscard]] std::set<std::filesystem::path> const& held() const noexcept { return *_held; }

    /// @param locking What its locks do.
    explicit FakeFileLocker(FakeLocking locking = FakeLocking::Works) noexcept: _locking { locking } {}

    [[nodiscard]] std::expected<std::unique_ptr<FileLock>, LockError> tryLockExclusive(
        std::filesystem::path const& lockFile) const override
    {
        if (_locking == FakeLocking::FailsWithIo)
            return std::unexpected(LockError::Io);
        if (!_held->insert(lockFile).second)
            return std::unexpected(LockError::Held);
        return std::make_unique<HeldLock>(_held, lockFile);
    }

  private:
    /// Releases its path on destruction; shares the set, so it may outlive the locker.
    class HeldLock final: public FileLock
    {
      public:
        HeldLock(std::shared_ptr<std::set<std::filesystem::path>> held, std::filesystem::path path):
            _held { std::move(held) }, _path { std::move(path) }
        {
        }
        HeldLock(HeldLock const&) = delete;
        HeldLock(HeldLock&&) = delete;
        HeldLock& operator=(HeldLock const&) = delete;
        HeldLock& operator=(HeldLock&&) = delete;
        ~HeldLock() override { _held->erase(_path); }

      private:
        std::shared_ptr<std::set<std::filesystem::path>> _held;
        std::filesystem::path _path;
    };

    std::shared_ptr<std::set<std::filesystem::path>> _held =
        std::make_shared<std::set<std::filesystem::path>>();
    FakeLocking _locking;
};

} // namespace vtextension::testing
