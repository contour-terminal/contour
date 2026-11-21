// SPDX-License-Identifier: Apache-2.0
#include <vtpty/BeneathRead.hpp>
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/OwnedFd.hpp>

#include <cerrno>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace vtpty
{

namespace
{
    /// Windows has no directory handles (nativeDirectoryOpener()): nothing is opened or looked at.
    ///
    /// The design a Windows reader is to follow, once directory handles exist there: each component opened
    /// relative to its parent's handle (NtCreateFile with a RootDirectory) with FILE_FLAG_OPEN_REPARSE_POINT,
    /// any reparse point refused, the file's nNumberOfLinks == 1, its volume serial equal to the root's, and
    /// GetFileType() == FILE_TYPE_DISK.
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

    /// Windows has no directory handle to read beneath: every read answers `Unsupported`.
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

IBeneathReader& nativeBeneathReader() noexcept
{
    static auto instance = UnsupportedBeneathReader {};
    return instance;
}

} // namespace vtpty
