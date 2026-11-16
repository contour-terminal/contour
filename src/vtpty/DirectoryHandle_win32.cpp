// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace vtpty
{

namespace
{
    /// Windows has no directory handle a child can be started in: every call answers `Unsupported`.
    class UnsupportedDirectoryOpener final: public IDirectoryOpener
    {
      public:
        [[nodiscard]] std::expected<DirectoryHandle, DirectoryError> open(std::string_view /*path*/) override
        {
            return std::unexpected(DirectoryError::Unsupported);
        }

        [[nodiscard]] std::expected<std::string, DirectoryError> canonical(
            std::string_view /*existingPath*/) override
        {
            return std::unexpected(DirectoryError::Unsupported);
        }

        [[nodiscard]] std::expected<void, DirectoryError> ensureExists(std::string_view /*path*/) override
        {
            return std::unexpected(DirectoryError::Unsupported);
        }
    };
} // namespace

IDirectoryOpener& nativeDirectoryOpener() noexcept
{
    static auto instance = UnsupportedDirectoryOpener {};
    return instance;
}

} // namespace vtpty
