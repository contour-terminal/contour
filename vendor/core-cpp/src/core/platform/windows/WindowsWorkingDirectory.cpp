// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>
#include <core/platform/PathUtils.hpp>
#include <core/platform/WorkingDirectory.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace core::platform
{

namespace
{
    /// The process's working directory, through std::filesystem, which speaks the wide API here.
    class WindowsWorkingDirectory final: public WorkingDirectory
    {
      public:
        [[nodiscard]] std::expected<void, PlatformError> changeDirectory(
            std::filesystem::path const& path) override
        {
            auto ec = std::error_code {};
            std::filesystem::current_path(path, ec);
            if (ec)
                return std::unexpected(PlatformError::IoError);
            return {};
        }

        [[nodiscard]] std::filesystem::path currentDirectory() const override
        {
            // Report the real on-disk capitalization (and an upper-case drive letter) so that
            // PWD, and whatever shows the user the directory, agree with how it is actually stored,
            // rather than echoing whatever case was passed to SetCurrentDirectory. That spelling
            // is UTF-8, so it goes back into a path as UTF-8, never through the code page -- and
            // UTF-8 by construction, spelled from a path, so a failure would be a defect here.
            return core::pathFromTrustedUtf8(canonicalCasePath(std::filesystem::current_path()));
        }
    };
} // namespace

std::unique_ptr<WorkingDirectory> nativeWorkingDirectory()
{
    return std::make_unique<WindowsWorkingDirectory>();
}

} // namespace core::platform
