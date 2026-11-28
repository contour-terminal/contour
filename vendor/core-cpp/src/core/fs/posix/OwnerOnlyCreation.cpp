// SPDX-License-Identifier: Apache-2.0
//
// Owner-only creation on POSIX. Direct where the mode alone decides (Linux); staged where an
// inherited ACL could widen a new object before it is stripped (macOS, FreeBSD):
//
//   1. a fresh staging directory is made beside the target, 0700, and through its handle stripped
//      of what it inherited and verified owner-only -- before anything is created inside it, so a
//      user who opened it in the meantime holds a descriptor through which the kernel, checking
//      search permission at each lookup, finds nothing;
//   2. the object is created inside it, where nothing can be inherited, and stripped and verified
//      too;
//   3. it is moved to its final name: a file by linkat, which fails where the name exists, as
//      O_EXCL does; a directory by renameat over an empty placeholder made with the final name by
//      mkdirat, which is what fails where the name exists, and which is stripped and verified as
//      well, so that what it inherited lets nobody leave an entry in it under the final name;
//   4. the staging directory is removed.
//
// A staging name is an HMAC of a counter under a key drawn once per file system, so nobody can make
// one in advance to stand in the way. Every step that fails removes what the earlier ones made,
// through the parent's handle.
#include <core/fs/posix/OwnerOnlyCreation.hpp>

#include <core/Sha256.hpp>
#include <core/fs/AccessScope.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace core::fs::detail
{

namespace
{

    constexpr ::mode_t OwnerOnlyFileMode = 0600;
    constexpr ::mode_t OwnerOnlyDirectoryMode = 0700;

    /// The name of the object inside its staging directory, which holds nothing else.
    constexpr auto StagedName = ".object";

    /// How many staging names are tried before giving up: each one that exists (another flow's,
    /// or one an attacker made) is skipped, never used.
    constexpr auto MaxStagingAttempts = 64;

    /// How many bytes of a staging name's HMAC its hex spells.
    constexpr auto StagingNameBytes = std::size_t { 16 };

    /// A staging directory: its name beneath the parent, and its handle.
    struct Staging
    {
        std::string name;
        UniqueHandle handle;
    };

    [[nodiscard]] UniqueHandle openDirectory(int directory, char const* name)
    {
        return UniqueHandle { retryOnInterrupt(
            [&] { return ::openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); }) };
    }

    /// What a staging directory still holds when it is removed.
    enum class Leftover : std::uint8_t
    {
        Nothing,   ///< The object was never made, or has been renamed out.
        File,      ///< A file, unlinked (linked to its final name, or a failure's).
        Directory, ///< A directory, removed.
    };

    /// Removes @p staging and what @p leftover says it holds. Best effort: a staging directory
    /// someone filled before its strip stays behind, owner-only.
    void removeStaging(int directory, Staging const& staging, Leftover leftover)
    {
        if (leftover != Leftover::Nothing)
            ::unlinkat(staging.handle.get(), StagedName, leftover == Leftover::Directory ? AT_REMOVEDIR : 0);
        ::unlinkat(directory, staging.name.c_str(), AT_REMOVEDIR);
    }

    /// A refusal to go on with an object that does not read owner-only.
    [[nodiscard]] std::unexpected<FsError> notOwnerOnly()
    {
        return std::unexpected { FsError { .code = FsErrorCode::NotPermitted,
                                           .systemCode = 0,
                                           .context = "the new object is not owner-only" } };
    }

    /// Opens the directory just made as @p name beneath @p directory and holds it to owner-only.
    [[nodiscard]] FsResult<void> holdDirectory(OwnerOnlyCreator const& creator,
                                               int directory,
                                               char const* name)
    {
        auto const made = openDirectory(directory, name);
        if (made.get() < 0)
            return errnoError("openat(O_DIRECTORY)");
        return creator.holdOwnerOnly(made.get(), OwnerOnlyDirectoryMode);
    }

    /// Makes a staging directory beneath @p directory, owner-only before it is returned.
    [[nodiscard]] FsResult<Staging> makeStaging(OwnerOnlyCreator const& creator, int directory)
    {
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, MaxStagingAttempts))
        {
            auto name = creator.nextStagingName();
            if (::mkdirat(directory, name.c_str(), OwnerOnlyDirectoryMode) != 0)
            {
                if (errno == EEXIST)
                    continue;
                return errnoError("mkdirat(staging)");
            }
            auto handle = openDirectory(directory, name.c_str());
            if (handle.get() < 0)
            {
                auto failed = errnoError("openat(staging)");
                ::unlinkat(directory, name.c_str(), AT_REMOVEDIR);
                return failed;
            }
            if (auto held = creator.holdOwnerOnly(handle.get(), OwnerOnlyDirectoryMode); !held)
            {
                ::unlinkat(directory, name.c_str(), AT_REMOVEDIR);
                return std::unexpected { held.error() };
            }
            return Staging { .name = std::move(name), .handle = std::move(handle) };
        }
        return std::unexpected { FsError {
            .code = FsErrorCode::Exist, .systemCode = EEXIST, .context = "no free staging name" } };
    }

    [[nodiscard]] FsResult<UniqueHandle> createFileDirect(OwnerOnlyCreator const& creator,
                                                          int directory,
                                                          std::string const& name,
                                                          int flags)
    {
        auto file = UniqueHandle { retryOnInterrupt(
            [&] { return ::openat(directory, name.c_str(), flags, OwnerOnlyFileMode); }) };
        if (file.get() < 0)
            return errnoError("openat");
        if (auto held = creator.holdOwnerOnly(file.get(), OwnerOnlyFileMode); !held)
        {
            // By name, which another process could have replaced since: reached only where the file
            // system ignores the mode it was given (vfat or exfat under fmask, cifs under noperm),
            // since Linux's mode caps every ACL entry. Whatever stands there was made under O_EXCL by
            // this call or renamed over it by someone who could write the directory anyway.
            ::unlinkat(directory, name.c_str(), 0);
            return std::unexpected { held.error() };
        }
        return file;
    }

    [[nodiscard]] FsResult<UniqueHandle> createFileStaged(OwnerOnlyCreator const& creator,
                                                          int directory,
                                                          std::string const& name,
                                                          int flags)
    {
        auto staging = makeStaging(creator, directory);
        if (!staging)
            return std::unexpected { staging.error() };
        auto file = UniqueHandle { retryOnInterrupt(
            [&] { return ::openat(staging->handle.get(), StagedName, flags, OwnerOnlyFileMode); }) };
        if (file.get() < 0)
        {
            auto failed = errnoError("openat(staged)");
            removeStaging(directory, *staging, Leftover::Nothing);
            return failed;
        }
        auto const held =
            creator.holdOwnerOnly(file.get(), OwnerOnlyFileMode).and_then([&]() -> FsResult<void> {
                // linkat, not renameat: it refuses a name that exists, as O_EXCL does.
                if (::linkat(staging->handle.get(), StagedName, directory, name.c_str(), 0) != 0)
                    return errnoError("linkat");
                return {};
            });
        removeStaging(directory, *staging, Leftover::File);
        if (!held)
            return std::unexpected { held.error() };
        return file;
    }

    [[nodiscard]] FsResult<void> createDirectoryDirect(OwnerOnlyCreator const& creator,
                                                       int directory,
                                                       std::string const& name)
    {
        if (::mkdirat(directory, name.c_str(), OwnerOnlyDirectoryMode) != 0)
            return errnoError("mkdirat");
        auto held = holdDirectory(creator, directory, name.c_str());
        // By name, as a direct file's: reached where the file system ignores the mode (vfat, exfat,
        // cifs under noperm). AT_REMOVEDIR removes nothing that is not an empty directory.
        if (!held)
            ::unlinkat(directory, name.c_str(), AT_REMOVEDIR);
        return held;
    }

    [[nodiscard]] FsResult<void> createDirectoryStaged(OwnerOnlyCreator const& creator,
                                                       int directory,
                                                       std::string const& name)
    {
        auto staging = makeStaging(creator, directory);
        if (!staging)
            return std::unexpected { staging.error() };
        if (::mkdirat(staging->handle.get(), StagedName, OwnerOnlyDirectoryMode) != 0)
        {
            auto failed = errnoError("mkdirat(staged)");
            removeStaging(directory, *staging, Leftover::Nothing);
            return failed;
        }
        if (auto held = holdDirectory(creator, staging->handle.get(), StagedName); !held)
        {
            removeStaging(directory, *staging, Leftover::Directory);
            return held;
        }
        // The placeholder is what refuses an existing name; the rename replaces it, empty, at once.
        // It is held to owner-only first: what it inherited could otherwise let another user leave
        // an entry in it, which would fail the rename and keep their entry under the final name.
        if (::mkdirat(directory, name.c_str(), OwnerOnlyDirectoryMode) != 0)
        {
            auto failed = errnoError("mkdirat");
            removeStaging(directory, *staging, Leftover::Directory);
            return failed;
        }
        if (auto held = holdDirectory(creator, directory, name.c_str()); !held)
        {
            ::unlinkat(directory, name.c_str(), AT_REMOVEDIR);
            removeStaging(directory, *staging, Leftover::Directory);
            return held;
        }
        if (::renameat(staging->handle.get(), StagedName, directory, name.c_str()) != 0)
        {
            auto failed = errnoError("renameat");
            ::unlinkat(directory, name.c_str(), AT_REMOVEDIR);
            removeStaging(directory, *staging, Leftover::Directory);
            return failed;
        }
        removeStaging(directory, *staging, Leftover::Nothing);
        return {};
    }

} // namespace

int systemStatHandle(int handle, struct ::stat* status)
{
    return ::fstat(handle, status);
}

OwnerOnlyCreator::OwnerOnlyCreator(IRandom& random, OwnerOnlyCreation how, StatHandle statHandle):
    _how { how }, _statHandle { std::move(statHandle) }
{
    random.fill(_stagingKey);
}

std::string OwnerOnlyCreator::nextStagingName() const
{
    auto const count = _stagingCounter.fetch_add(1, std::memory_order_relaxed);
    auto message = std::array<std::byte, sizeof(count)> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, message.size()))
        message[index] = static_cast<std::byte>(count >> (8U * index));
    auto const tag = hmacSha256(_stagingKey, message);
    constexpr auto Digits = std::string_view { "0123456789abcdef" };
    auto name = std::string { ".core-cpp-owner-only." };
    for (auto const byte: std::span { tag }.first(StagingNameBytes))
    {
        name += Digits[std::to_integer<unsigned>(byte) >> 4U];
        name += Digits[std::to_integer<unsigned>(byte) & 0xFU];
    }
    return name;
}

FsResult<void> OwnerOnlyCreator::holdOwnerOnly(int handle, ::mode_t mode) const
{
    return stripInheritedAcl(handle, mode)
        .and_then([&]() -> FsResult<struct ::stat> {
            struct ::stat status {};
            if (_statHandle(handle, &status) != 0)
                return errnoError("fstat");
            return status;
        })
        .and_then([&](struct ::stat const& status) -> FsResult<void> {
            if (accessFactsOf(handle, status).transform(classifyAccess) == AccessScope::OwnerOnly)
                return {};
            return notOwnerOnly();
        });
}

FsResult<UniqueHandle> OwnerOnlyCreator::createFile(int directory, std::string const& name, int flags) const
{
    return _how == OwnerOnlyCreation::Staged ? createFileStaged(*this, directory, name, flags)
                                             : createFileDirect(*this, directory, name, flags);
}

FsResult<void> OwnerOnlyCreator::createDirectory(int directory, std::string const& name) const
{
    return _how == OwnerOnlyCreation::Staged ? createDirectoryStaged(*this, directory, name)
                                             : createDirectoryDirect(*this, directory, name);
}

} // namespace core::fs::detail
