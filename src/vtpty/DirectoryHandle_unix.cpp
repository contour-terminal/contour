// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/OwnedFd.hpp>

#include <sys/stat.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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

    /// Opens @p canonical, an absolute path without links, one component at a time from `/`, following none:
    /// a component swapped for a link meanwhile is refused, so the descriptor is at @p canonical.
    [[nodiscard]] std::expected<OwnedFd, DirectoryError> openWalking(std::string const& canonical)
    {
        constexpr auto Flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        auto current = OwnedFd { ::open("/", Flags) };
        if (!current.isOpen())
            return std::unexpected(directoryError(errno));
        for (auto const component: std::string_view { canonical } | std::views::split('/'))
        {
            auto const name = std::string { component.begin(), component.end() };
            if (name.empty())
                continue;
            auto next = OwnedFd { ::openat(current.get(), name.c_str(), Flags) };
            if (!next.isOpen())
                // ELOOP (EMLINK on FreeBSD) here is a link where realpath() saw a directory: the name changed
                // meanwhile.
                return std::unexpected(errno == ELOOP || errno == EMLINK ? DirectoryError::Missing
                                                                         : directoryError(errno));
            current = std::move(next);
        }
        return current;
    }

    /// @return Whether the open directory @p descriptor has been removed (no name links to it any more).
    [[nodiscard]] bool isRemoved(int descriptor) noexcept
    {
        struct stat status {};
        return ::fstat(descriptor, &status) == 0 && status.st_nlink == 0;
    }

    class NativeDirectoryOpener final: public IDirectoryOpener
    {
      public:
        explicit NativeDirectoryOpener(DescriptorPaths paths) noexcept: _paths { paths } {}

        [[nodiscard]] std::expected<DirectoryHandle, DirectoryError> open(std::string_view path) override
        {
            auto const name = std::string { path };
            auto descriptor = OwnedFd { ::open(name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
            if (!descriptor.isOpen())
                return std::unexpected(directoryError(errno));
            if (isRemoved(descriptor.get()))
                return std::unexpected(DirectoryError::Missing);
            // Read from the descriptor, so that the path is the directory's, not the name's at a later
            // moment.
            if (_paths == DescriptorPaths::FromKernel)
                if (auto resolved = pathOfDescriptor(descriptor.get()))
                    return DirectoryHandle { std::move(descriptor), std::move(*resolved) };
            // No path from the descriptor: the canonical name is opened again without following a link, so
            // the handle is at the path it is given, never at what the name led to a moment earlier.
            auto real = realPath(name);
            if (!real)
                return std::unexpected(real.error());
            auto walked = openWalking(*real);
            if (!walked)
                return std::unexpected(walked.error());
            if (isRemoved(walked->get()))
                return std::unexpected(DirectoryError::Missing);
            return DirectoryHandle { std::move(*walked), std::move(*real) };
        }

        [[nodiscard]] std::expected<std::string, DirectoryError> canonical(
            std::string_view existingPath) override
        {
            return realPath(std::string { existingPath });
        }

        [[nodiscard]] std::expected<DirectoryHandle, DirectoryError> createBeneath(
            DirectoryHandle const& parent, std::string_view name) override
        {
            if (name.empty() || name == "." || name == ".." || name.contains('/') || name.contains('\0'))
                return std::unexpected(DirectoryError::AccessDenied);
            auto const component = std::string { name };
            if (::mkdirat(parent.descriptor(), component.c_str(), 0777) != 0 && errno != EEXIST)
                return std::unexpected(directoryError(errno));
            auto descriptor = OwnedFd { ::openat(
                parent.descriptor(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) };
            if (!descriptor.isOpen())
                // ELOOP here is a symlink in the name's place, which is never followed.
                return std::unexpected(errno == ELOOP ? DirectoryError::NotADirectory
                                                      : directoryError(errno));
            auto resolved = _paths == DescriptorPaths::FromKernel ? pathOfDescriptor(descriptor.get())
                                                                  : std::optional<std::string> {};
            if (!resolved)
                resolved =
                    parent.resolvedPath() == "/" ? "/" + component : parent.resolvedPath() + "/" + component;
            return DirectoryHandle { std::move(descriptor), std::move(*resolved) };
        }

      private:
        DescriptorPaths _paths;
    };
} // namespace

std::unique_ptr<IDirectoryOpener> makeDirectoryOpener(DescriptorPaths paths)
{
    return std::make_unique<NativeDirectoryOpener>(paths);
}

IDirectoryOpener& nativeDirectoryOpener() noexcept
{
    static auto instance = NativeDirectoryOpener { DescriptorPaths::FromKernel };
    return instance;
}

bool DirectoryHandle::isRemoved() const noexcept
{
    return descriptor() >= 0 && vtpty::isRemoved(descriptor());
}

} // namespace vtpty
