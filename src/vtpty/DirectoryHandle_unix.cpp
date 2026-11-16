// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/OwnedFd.hpp>

#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace vtpty
{

namespace
{
    /// @return What @p error, an errno value from opening or resolving a path, says about it.
    [[nodiscard]] DirectoryError directoryError(int error) noexcept
    {
        switch (error)
        {
            case ENOENT:
            case ELOOP:
            case ENAMETOOLONG: return DirectoryError::Missing;
            case ENOTDIR: return DirectoryError::NotADirectory;
            default: return DirectoryError::AccessDenied;
        }
    }

    /// @return The canonical form of @p path, by realpath(); or why it has none.
    [[nodiscard]] std::expected<std::string, DirectoryError> realPath(std::string const& path)
    {
        auto const resolved =
            std::unique_ptr<char, decltype(&std::free)> { ::realpath(path.c_str(), nullptr), &std::free };
        if (!resolved)
            return std::unexpected(directoryError(errno));
        return std::string { resolved.get() };
    }

    /// @return The path the open directory @p descriptor is at now, as the kernel tells it; std::nullopt
    ///         where it cannot tell.
    [[nodiscard]] std::optional<std::string> pathOfDescriptor(int descriptor)
    {
#ifdef __linux__
        auto buffer = std::array<char, PATH_MAX> {};
        auto const link = std::format("/proc/self/fd/{}", descriptor);
        auto const length = ::readlink(link.c_str(), buffer.data(), buffer.size());
        if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size())
            return std::nullopt;
        return std::string { buffer.data(), static_cast<std::size_t>(length) };
#elifdef F_GETPATH
        auto buffer = std::array<char, PATH_MAX> {};
        if (::fcntl(descriptor, F_GETPATH, buffer.data()) != 0)
            return std::nullopt;
        return std::string { buffer.data() };
#else
        (void) descriptor;
        return std::nullopt;
#endif
    }

    class NativeDirectoryOpener final: public IDirectoryOpener
    {
      public:
        [[nodiscard]] std::expected<DirectoryHandle, DirectoryError> open(std::string_view path) override
        {
            auto const name = std::string { path };
            auto descriptor = OwnedFd { ::open(name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
            if (!descriptor.isOpen())
                return std::unexpected(directoryError(errno));
            // Read from the descriptor, so that the path is the directory's, not the name's at a later
            // moment.
            auto resolved = pathOfDescriptor(descriptor.get());
            if (!resolved)
            {
                auto real = realPath(name);
                if (!real)
                    return std::unexpected(real.error());
                resolved = std::move(*real);
            }
            return DirectoryHandle { std::move(descriptor), std::move(*resolved) };
        }

        [[nodiscard]] std::expected<std::string, DirectoryError> canonical(
            std::string_view existingPath) override
        {
            return realPath(std::string { existingPath });
        }

        [[nodiscard]] std::expected<void, DirectoryError> ensureExists(std::string_view path) override
        {
            auto const directory = std::filesystem::path { path };
            auto error = std::error_code {};
            std::filesystem::create_directories(directory, error);
            if (error && error != std::errc::file_exists)
                return std::unexpected(directoryError(error.value()));
            if (!std::filesystem::is_directory(directory, error))
                return std::unexpected(DirectoryError::NotADirectory);
            return {};
        }
    };
} // namespace

IDirectoryOpener& nativeDirectoryOpener() noexcept
{
    static auto instance = NativeDirectoryOpener {};
    return instance;
}

} // namespace vtpty
