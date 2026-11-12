// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <vthost/FilePrivacy.hpp>

    #include <core/Utils.hpp>

    #include <sys/stat.h>

    #include <array>
    #include <cerrno>
    #include <format>
    #include <system_error>

    #include <fcntl.h>
    #include <unistd.h>

namespace vthost
{

std::expected<void, std::string> NativeFilePrivacy::restrictToOwner(std::filesystem::path const& path,
                                                                    PrivateEntry kind) const
{
    namespace fs = std::filesystem;
    auto const permissions = kind == PrivateEntry::Directory ? fs::perms::owner_all
                                                             : fs::perms::owner_read | fs::perms::owner_write;
    auto error = std::error_code {};
    fs::permissions(path, permissions, fs::perm_options::replace, error);
    if (error)
        return std::unexpected(std::format("cannot restrict {}: {}", path.string(), error.message()));
    return {};
}

std::expected<std::string, std::string> NativeFilePrivacy::readPrivateFile(
    std::filesystem::path const& path) const
{
    auto const failure = [&](std::string_view what) {
        return std::unexpected(std::format("cannot {} '{}': {}",
                                           what,
                                           path.string(),
                                           std::error_code { errno, std::generic_category() }.message()));
    };
    // Not through a link: a link planted in its place would have the check made on its target.
    auto const fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return failure("open");
    auto const closed = core::Finally { [fd] { ::close(fd); } };
    struct stat status {};
    if (::fstat(fd, &status) != 0)
        return failure("inspect");
    if (!S_ISREG(status.st_mode))
        return std::unexpected(std::format("'{}' is not a regular file", path.string()));
    if (auto checked =
            checkPrivateFile(FileOwnership { .ownerId = status.st_uid,
                                             .mode = static_cast<std::uint32_t>(status.st_mode & 07777) },
                             ::geteuid(),
                             path);
        !checked)
        return std::unexpected(std::move(checked.error()));
    auto content = std::string {};
    auto buffer = std::array<char, 4096> {};
    while (true)
    {
        auto const count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            return failure("read");
        }
        if (count == 0)
            return content;
        content.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

} // namespace vthost

#endif
