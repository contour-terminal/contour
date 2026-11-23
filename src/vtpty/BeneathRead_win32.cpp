// SPDX-License-Identifier: Apache-2.0
#include <vtpty/BeneathRead.hpp>
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/OwnedFd.hpp>
#include <vtpty/UniqueHandle.hpp>
#include <vtpty/Win32BeneathRead.hpp>
#include <vtpty/WindowsPathRules.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>

namespace vtpty
{

namespace
{
    /// The POSIX seam, which Windows cannot serve: every call fails with `ENOSYS`.
    class UnsupportedBeneathSystem final: public IBeneathSystem
    {
      public:
        [[nodiscard]] std::expected<OwnedFd, int> open(int /*directory*/,
                                                       std::string const& /*name*/,
                                                       BeneathTarget /*target*/) override
        {
            return std::unexpected(ENOSYS);
        }

        [[nodiscard]] std::expected<FileStatus, int> statusAt(int /*directory*/,
                                                              std::string const& /*name*/) override
        {
            return std::unexpected(ENOSYS);
        }

        [[nodiscard]] std::expected<FileStatus, int> status(int /*descriptor*/) override
        {
            return std::unexpected(ENOSYS);
        }
    };

    /// A reader over the POSIX seam, which Windows cannot serve: every read answers `Unsupported`.
    class UnsupportedBeneathReader final: public IBeneathReader
    {
      public:
        [[nodiscard]] std::expected<BeneathFile, BeneathReadError> read(
            DirectoryHandle const& /*root*/,
            std::span<std::string_view const> /*components*/,
            std::size_t /*maxBytes*/,
            std::span<FileIdentity const> /*fenced*/) override
        {
            return std::unexpected(BeneathReadError::Unsupported);
        }

        [[nodiscard]] std::expected<FileIdentity, BeneathReadError> identityOf(
            DirectoryHandle const& /*root*/, std::span<std::string_view const> /*components*/) override
        {
            return std::unexpected(BeneathReadError::Unsupported);
        }
    };

    /// Which step of the walk failed.
    enum class Step : uint8_t
    {
        Directory, ///< A directory above the file.
        File,      ///< The file itself, looked at.
    };

    /// What a Win32 error from one step of the walk means.
    struct Win32ErrorRow
    {
        Step step;
        DWORD error;
        BeneathReadError meaning;
    };

    /// Every Win32 error the walk answers by name; any other is `Unreadable`, never `DirectoryMissing`.
    // clang-format off
    constexpr auto Win32ErrorRows = std::to_array<Win32ErrorRow>({
        { .step = Step::Directory, .error = ERROR_FILE_NOT_FOUND, .meaning = BeneathReadError::DirectoryMissing },
        { .step = Step::Directory, .error = ERROR_PATH_NOT_FOUND, .meaning = BeneathReadError::DirectoryMissing },
        // A file where a directory is.
        { .step = Step::Directory, .error = ERROR_DIRECTORY,      .meaning = BeneathReadError::NotRegular },
        { .step = Step::Directory, .error = ERROR_ACCESS_DENIED,  .meaning = BeneathReadError::Unreadable },
        { .step = Step::File,      .error = ERROR_FILE_NOT_FOUND, .meaning = BeneathReadError::NotRegular },
        { .step = Step::File,      .error = ERROR_PATH_NOT_FOUND, .meaning = BeneathReadError::NotRegular },
        { .step = Step::File,      .error = ERROR_ACCESS_DENIED,  .meaning = BeneathReadError::Unreadable },
    });
    // clang-format on

    /// @return What @p error at @p step means.
    [[nodiscard]] BeneathReadError meaningOf(Step step, DWORD error) noexcept
    {
        auto const row = std::ranges::find_if(Win32ErrorRows, [&](Win32ErrorRow const& candidate) {
            return candidate.step == step && candidate.error == error;
        });
        return row == Win32ErrorRows.end() ? BeneathReadError::Unreadable : row->meaning;
    }

    /// @return Whether @p identity is no identity at all: a zero file id, which some SMB servers report for
    ///         every file, and which would match every fence or none.
    [[nodiscard]] bool isUnknown(FileIdentity const& identity) noexcept
    {
        return identity.inode == 0 && identity.inodeHigh == 0;
    }

    /// @return @p components as the UTF-16 names they are opened by; `Escapes` when one is not one plain
    ///         Windows name (isPlainWindowsName()) or not UTF-8.
    [[nodiscard]] std::expected<std::vector<std::wstring>, BeneathReadError> windowsNamesOf(
        std::span<std::string_view const> components)
    {
        auto names = std::vector<std::wstring> {};
        names.reserve(components.size());
        for (auto const component: components)
        {
            if (!isPlainWindowsName(component))
                return std::unexpected(BeneathReadError::Escapes);
            auto name = core::pathFromUtf8(component);
            if (!name)
                return std::unexpected(BeneathReadError::Escapes);
            names.push_back(name->native());
        }
        return names;
    }

    /// The directory a walk ended in.
    struct Walked
    {
        UniqueHandle directory; ///< The last directory opened; none when the walk was the root itself.
        FileIdentity identity;  ///< Which directory it is.
    };

    /// Reads a file beneath a held directory through the Win32 calls of IWin32BeneathSystem, step for step
    /// as the POSIX reader does through IBeneathSystem.
    class Win32BeneathReader final: public IBeneathReader
    {
      public:
        explicit Win32BeneathReader(IWin32BeneathSystem& system) noexcept: _system { system } {}

        [[nodiscard]] std::expected<BeneathFile, BeneathReadError> read(
            DirectoryHandle const& root,
            std::span<std::string_view const> components,
            std::size_t maxBytes,
            std::span<FileIdentity const> fenced) override
        {
            if (components.empty())
                return std::unexpected(BeneathReadError::Escapes);
            auto const names = windowsNamesOf(components);
            if (!names)
                return std::unexpected(names.error());
            auto const rootIdentity = identityOfRoot(root);
            if (!rootIdentity)
                return std::unexpected(rootIdentity.error());
            auto const directories = std::span { *names }.first(names->size() - 1);
            auto const walked = walk(root.nativeHandle(), *rootIdentity, directories, fenced);
            if (!walked)
                return std::unexpected(walked.error());
            auto* const directory = walked->directory.valid() ? walked->directory.get() : root.nativeHandle();

            // The look: attribute access only, so nothing that is no regular file on disk is ever opened for
            // its data (a placeholder's is never recalled).
            auto const& name = names->back();
            auto const looked = _system.open(directory, name, BeneathEntry::Entry);
            if (!looked)
                return std::unexpected(openFailure(Step::File, directory, name, looked.error()));
            auto const lookedStatus = _system.status(looked->get());
            if (!lookedStatus)
                return std::unexpected(BeneathReadError::Unreadable);
            if (lookedStatus->kind == FileKind::Symlink)
                return std::unexpected(BeneathReadError::Escapes);
            if (lookedStatus->kind != FileKind::Regular)
                return std::unexpected(BeneathReadError::NotRegular);

            // The same object, re-opened by its handle: no name is resolved twice. Its description is the
            // authority, as fstat is on POSIX.
            auto const file = _system.reopenForRead(looked->get());
            if (!file)
                return std::unexpected(BeneathReadError::Unreadable);
            auto const status = _system.status(file->get());
            if (!status)
                return std::unexpected(BeneathReadError::Unreadable);
            if (status->kind != FileKind::Regular)
                return std::unexpected(BeneathReadError::NotRegular);
            // A second name may lie outside the root.
            if (status->links != 1 || status->identity.device != rootIdentity->device)
                return std::unexpected(BeneathReadError::Escapes);
            if (isUnknown(status->identity))
                return std::unexpected(BeneathReadError::Unreadable);
            return readUpTo(file->get(), status->sizeBytes, maxBytes).transform([&](std::string bytes) {
                return BeneathFile { .bytes = std::move(bytes), .modifiedUnixMs = status->modifiedUnixMs };
            });
        }

        [[nodiscard]] std::expected<FileIdentity, BeneathReadError> identityOf(
            DirectoryHandle const& root, std::span<std::string_view const> components) override
        {
            auto const names = windowsNamesOf(components);
            if (!names)
                return std::unexpected(names.error());
            auto const rootIdentity = identityOfRoot(root);
            if (!rootIdentity)
                return std::unexpected(rootIdentity.error());
            return walk(root.nativeHandle(), *rootIdentity, *names, {}).transform([](Walked const& walked) {
                return walked.identity;
            });
        }

      private:
        /// @return Which directory @p root is; `Unreadable` for an unbacked or unknown one,
        /// `DirectoryMissing`
        ///         once it was removed while held.
        [[nodiscard]] std::expected<FileIdentity, BeneathReadError> identityOfRoot(
            DirectoryHandle const& root)
        {
            // With no root, nothing may be opened relative to it.
            if (root.nativeHandle() == nullptr)
                return std::unexpected(BeneathReadError::Unreadable);
            auto const status = _system.status(root.nativeHandle());
            if (!status)
                return std::unexpected(BeneathReadError::Unreadable);
            // A root removed while it was held: what lay beneath it is gone with it.
            if (status->links == 0)
                return std::unexpected(BeneathReadError::DirectoryMissing);
            if (isUnknown(status->identity))
                return std::unexpected(BeneathReadError::Unreadable);
            return status->identity;
        }

        /// Opens @p directories beneath @p root, each by the handle above it: no name is ever resolved from
        /// the root's path. A link is `Escapes`, as is a directory on another volume than @p rootIdentity's;
        /// one of @p fenced is `Fenced`.
        /// @return The last directory opened and its identity; @p root's identity, and no handle, when
        ///         @p directories is empty.
        [[nodiscard]] std::expected<Walked, BeneathReadError> walk(HANDLE root,
                                                                   FileIdentity const& rootIdentity,
                                                                   std::span<std::wstring const> directories,
                                                                   std::span<FileIdentity const> fenced)
        {
            auto walked = Walked { .directory = UniqueHandle {}, .identity = rootIdentity };
            for (auto const& name: directories)
            {
                auto* const parent = walked.directory.valid() ? walked.directory.get() : root;
                auto next = _system.open(parent, name, BeneathEntry::Directory);
                if (!next)
                    return std::unexpected(openFailure(Step::Directory, parent, name, next.error()));
                auto const status = _system.status(next->get());
                if (!status || isUnknown(status->identity))
                    return std::unexpected(BeneathReadError::Unreadable);
                if (status->kind == FileKind::Symlink)
                    return std::unexpected(BeneathReadError::Escapes);
                // A directory whose storage no rule accepts (an unknown reparse tag): not walked into.
                if (status->kind != FileKind::Directory)
                    return std::unexpected(BeneathReadError::NotRegular);
                if (status->identity.device != rootIdentity.device)
                    return std::unexpected(BeneathReadError::Escapes);
                // Another root's directory, however the path spells it (another case, an 8.3 name). Compared
                // by any_of, not contains: MSVC's STL vectorizes the latter's find for a 24-byte type, which
                // clang-tidy's front end refuses.
                if (std::ranges::any_of(fenced,
                                        [&](FileIdentity const& fence) { return fence == status->identity; }))
                    return std::unexpected(BeneathReadError::Fenced);
                walked = Walked { .directory = std::move(*next), .identity = status->identity };
            }
            return walked;
        }

        /// @return Why the open of @p name beneath @p parent at @p step failed with @p error.
        [[nodiscard]] BeneathReadError openFailure(Step step,
                                                   HANDLE parent,
                                                   std::wstring_view name,
                                                   DWORD error)
        {
            // A file symlink where a directory is: a link is a link, whatever it points at (POSIX's ENOTDIR
            // re-look).
            if (error == ERROR_DIRECTORY)
                if (auto const looked = _system.open(parent, name, BeneathEntry::Entry))
                    if (auto const status = _system.status(looked->get());
                        status && status->kind == FileKind::Symlink)
                        return BeneathReadError::Escapes;
            // Nothing may be opened beneath a directory whose deletion is pending (STATUS_DELETE_PENDING).
            if (error == ERROR_ACCESS_DENIED)
                if (auto const status = _system.status(parent); status && status->links == 0)
                    return BeneathReadError::DirectoryMissing;
            return meaningOf(step, error);
        }

        /// Reads at most @p maxBytes + 1 bytes of @p file: the bound is what was read.
        /// @param sizeHint What the handle said the size was: how much is reserved, never how much is read.
        [[nodiscard]] std::expected<std::string, BeneathReadError> readUpTo(HANDLE file,
                                                                            uint64_t sizeHint,
                                                                            std::size_t maxBytes)
        {
            constexpr auto ChunkBytes = std::size_t { 16 } * 1024;
            auto const limit = maxBytes == std::numeric_limits<std::size_t>::max() ? maxBytes : maxBytes + 1;
            auto bytes = std::string {};
            bytes.reserve(static_cast<std::size_t>(std::min<uint64_t>(sizeHint, limit)));
            while (bytes.size() < limit)
            {
                auto const have = bytes.size();
                auto const want = std::min(ChunkBytes, limit - have);
                bytes.resize(have + want);
                auto const got = _system.read(file, std::span { bytes }.subspan(have, want));
                if (!got)
                    return std::unexpected(BeneathReadError::Unreadable);
                bytes.resize(have + *got);
                if (*got == 0)
                    break;
            }
            if (bytes.size() > maxBytes)
                return std::unexpected(BeneathReadError::TooLarge);
            return bytes;
        }

        IWin32BeneathSystem& _system;
    };
} // namespace

std::unique_ptr<IBeneathSystem> makeBeneathSystem(BeneathResolution /*resolution*/)
{
    return std::make_unique<UnsupportedBeneathSystem>();
}

IBeneathSystem& nativeBeneathSystem() noexcept
{
    static auto instance = UnsupportedBeneathSystem {};
    return instance;
}

std::unique_ptr<IBeneathReader> makeBeneathReader(IBeneathSystem& /*system*/)
{
    return std::make_unique<UnsupportedBeneathReader>();
}

std::unique_ptr<IBeneathReader> makeWin32BeneathReader(IWin32BeneathSystem& system)
{
    return std::make_unique<Win32BeneathReader>(system);
}

IBeneathReader& nativeBeneathReader() noexcept
{
    static auto instance = Win32BeneathReader { nativeWin32BeneathSystem() };
    return instance;
}

} // namespace vtpty
