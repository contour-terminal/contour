// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyMediumSource.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace vtbackend::kitty_graphics
{

namespace
{
    /// Closes a descriptor when it goes out of scope.
    class FileDescriptor
    {
      public:
        explicit FileDescriptor(int fd) noexcept: _fd { fd } {}
        ~FileDescriptor()
        {
            if (_fd >= 0)
                ::close(_fd);
        }
        FileDescriptor(FileDescriptor const&) = delete;
        FileDescriptor(FileDescriptor&&) = delete;
        FileDescriptor& operator=(FileDescriptor const&) = delete;
        FileDescriptor& operator=(FileDescriptor&&) = delete;

        [[nodiscard]] int get() const noexcept { return _fd; }

      private:
        int _fd;
    };

    /// @return @p path with every symbolic link resolved, or nullopt when it does not resolve (missing,
    ///         a loop, a component that is not a directory...). Resolving opens nothing.
    [[nodiscard]] std::optional<std::filesystem::path> canonicalPath(std::string const& path)
    {
        auto const resolved =
            std::unique_ptr<char, decltype(&std::free)> { ::realpath(path.c_str(), nullptr), &std::free };
        if (!resolved)
            return std::nullopt;
        return std::filesystem::path { resolved.get() };
    }

    /// @return The path the kernel holds for the opened descriptor @p fd, or nullopt where it cannot tell
    ///         (then the read is refused: an unverified descriptor is not read).
    [[nodiscard]] std::optional<std::filesystem::path> descriptorPath(int fd)
    {
#ifdef __linux__
        auto buffer = std::array<char, PATH_MAX + 1> {};
        auto const link = std::format("/proc/self/fd/{}", fd);
        auto const length = ::readlink(link.c_str(), buffer.data(), buffer.size());
        if (length <= 0 || static_cast<size_t>(length) >= buffer.size())
            return std::nullopt;
        return std::filesystem::path { std::string_view { buffer.data(), static_cast<size_t>(length) } };
#elifdef __APPLE__
        auto buffer = std::array<char, PATH_MAX> {};
        if (::fcntl(fd, F_GETPATH, buffer.data()) == -1)
            return std::nullopt;
        return std::filesystem::path { buffer.data() };
#else
        (void) fd;
        return std::nullopt;
#endif
    }

    /// @return What the policy judges of @p status.
    [[nodiscard]] OpenedFile openedFileOf(struct stat const& status) noexcept
    {
        return OpenedFile { .kind = S_ISREG(status.st_mode) ? FileKind::Regular : FileKind::Other,
                            .ownerId = static_cast<uint32_t>(status.st_uid),
                            .linkCount = static_cast<uint64_t>(status.st_nlink),
                            .size = static_cast<uint64_t>(status.st_size) };
    }

    /// Reads @p fd to its end, refusing a file that grew past @p limit since it was measured.
    [[nodiscard]] std::expected<std::vector<uint8_t>, MediaRefusal> readBounded(int fd,
                                                                                uint64_t expectedSize,
                                                                                size_t limit)
    {
        auto bytes = std::vector<uint8_t>(static_cast<size_t>(expectedSize));
        auto filled = size_t { 0 };
        while (true)
        {
            // One byte of room beyond the limit tells a file that grew past it from one that ends there.
            if (filled == bytes.size())
            {
                if (bytes.size() > limit)
                    return std::unexpected(MediaRefusal::TooLarge);
                bytes.resize(std::min(bytes.size() + 4096, limit + 1));
            }
            auto const count = ::read(fd, bytes.data() + filled, bytes.size() - filled);
            if (count < 0 && errno == EINTR)
                continue;
            // A regular file never answers EAGAIN; whatever does is not read on the terminal's thread.
            if (count < 0)
                return std::unexpected(MediaRefusal::Unreadable);
            if (count == 0)
                break;
            filled += static_cast<size_t>(count);
        }
        if (filled > limit)
            return std::unexpected(MediaRefusal::TooLarge);
        bytes.resize(filled);
        return bytes;
    }

    /// Unlinks @p path, but only while it still names the file @p read was: a file swapped in after the read
    /// is left alone.
    void unlinkIfSame(std::filesystem::path const& path, struct stat const& read) noexcept
    {
        struct stat current {};
        if (::lstat(path.c_str(), &current) == 0 && current.st_dev == read.st_dev
            && current.st_ino == read.st_ino)
            (void) ::unlink(path.c_str());
    }
} // namespace

std::expected<std::vector<uint8_t>, MediaRefusal> PosixMediumSource::read(std::string_view path,
                                                                          Medium medium,
                                                                          MediaPolicy const& policy) const
{
    // Refused before the filesystem is touched at all.
    if (auto const accepted = checkMedium(policy, medium); !accepted)
        return std::unexpected(accepted.error());
    if (path.empty() || path.contains('\0'))
        return std::unexpected(MediaRefusal::Unreadable);

    auto const canonical = canonicalPath(std::string { path });
    if (!canonical)
        return std::unexpected(MediaRefusal::Unreadable);
    if (auto const accepted = checkPath(policy, *canonical, medium); !accepted)
        return std::unexpected(accepted.error());

    if (_pathAccepted)
        _pathAccepted(*canonical);

    // O_NOFOLLOW refuses a final component swapped for a symbolic link; O_NONBLOCK keeps a FIFO from
    // blocking the open until a writer appears.
    auto const fd =
        FileDescriptor { ::open(canonical->c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) };
    if (fd.get() < 0)
        return std::unexpected(MediaRefusal::Unreadable);

    // A directory above the file swapped for a symbolic link is followed by open(): the descriptor then
    // names another file than the one the policy judged.
    if (descriptorPath(fd.get()) != *canonical)
        return std::unexpected(MediaRefusal::Unreadable);

    struct stat status {};
    if (::fstat(fd.get(), &status) != 0)
        return std::unexpected(MediaRefusal::Unreadable);
    if (auto const accepted = checkOpenedFile(policy, openedFileOf(status)); !accepted)
        return std::unexpected(accepted.error());

    auto bytes = readBounded(fd.get(), static_cast<uint64_t>(status.st_size), policy.sizeLimit);
    if (!bytes)
        return bytes;

    if (deletionAfterRead(policy, *canonical, medium) == DeletionAfterRead::Delete)
        unlinkIfSame(*canonical, status);
    return bytes;
}

} // namespace vtbackend::kitty_graphics
