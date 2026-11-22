// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyBoundedRead.hpp>
#include <vtbackend/graphics/KittyMediumSource.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
        FileDescriptor(FileDescriptor&& other) noexcept: _fd { std::exchange(other._fd, -1) } {}
        FileDescriptor& operator=(FileDescriptor const&) = delete;
        FileDescriptor& operator=(FileDescriptor&& other) noexcept
        {
            std::swap(_fd, other._fd);
            return *this;
        }

        [[nodiscard]] int get() const noexcept { return _fd; }

      private:
        int _fd;
    };

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

    /// @return What the policy judges of @p status, its owner compared with the reader @p readerId.
    [[nodiscard]] OpenedFile openedFileOf(struct stat const& status, uint32_t readerId) noexcept
    {
        return OpenedFile { .kind = S_ISREG(status.st_mode) ? FileKind::Regular : FileKind::Other,
                            .owner = static_cast<uint32_t>(status.st_uid) == readerId ? FileOwner::Reader
                                                                                      : FileOwner::Other,
                            .linkCount = static_cast<uint64_t>(status.st_nlink),
                            .size = static_cast<uint64_t>(status.st_size) };
    }

    /// Reads what fits into @p into from @p fd, as readBounded() asks.
    [[nodiscard]] std::expected<size_t, MediaRefusal> readSome(int fd, std::span<uint8_t> into)
    {
        while (true)
        {
            auto const count = ::read(fd, into.data(), into.size());
            if (count < 0 && errno == EINTR)
                continue;
            // A regular file never answers EAGAIN; whatever does is not read on the terminal's thread.
            if (count < 0)
                return std::unexpected(MediaRefusal::Unreadable);
            return static_cast<size_t>(count);
        }
    }

    /// Opens @p name below the directory @p directory without following a link, refusing one: the link itself
    /// is looked at (fstatat with AT_SYMLINK_NOFOLLOW), never its target.
    /// @param flags The open flags, O_NOFOLLOW and O_CLOEXEC added.
    [[nodiscard]] std::expected<FileDescriptor, MediaRefusal> openBeneath(int directory,
                                                                          std::filesystem::path const& name,
                                                                          int flags)
    {
        auto fd = FileDescriptor { ::openat(directory, name.c_str(), flags | O_NOFOLLOW | O_CLOEXEC) };
        if (fd.get() >= 0)
            return fd;
        struct stat status {};
        if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(status.st_mode))
            return std::unexpected(MediaRefusal::SymbolicLink);
        return std::unexpected(MediaRefusal::Unreadable);
    }

    /// What walking a path down from its root opened: the file, and the directory holding it.
    struct Walked
    {
        FileDescriptor directory;
        FileDescriptor file;
        std::filesystem::path path; ///< The root's real path and the components walked.
    };

    /// Opens @p requested one component at a time down from its root, following no link below the root: a
    /// link's target is never looked up, so no component reaches an automount or a network filesystem, nor
    /// leaves the root. The root itself is configuration, and its own links (macOS `/tmp`) are followed.
    [[nodiscard]] std::expected<Walked, MediaRefusal> walkBeneath(
        RequestedPath const& requested,
        MediaPolicy const& policy,
        Medium medium,
        MediumReadInterleavings const& interleavings)
    {
        auto directory =
            FileDescriptor { ::open(requested.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
        if (directory.get() < 0)
            return std::unexpected(MediaRefusal::Unreadable);
        auto walked = descriptorPath(directory.get());
        if (!walked)
            return std::unexpected(MediaRefusal::Unreadable);
        auto const& components = requested.belowRoot;

        // No link below the root is followed, so the real path is known once the root is open: judged before
        // anything below the root is opened -- a root's spelling re-pointed into /dev must open no device.
        auto real = *walked;
        for (auto const& component: components)
            real /= component;
        if (auto const accepted = checkPath(policy, real, medium); !accepted)
            return std::unexpected(accepted.error());

        for (auto const& component: components | std::views::take(components.size() - 1))
        {
            if (interleavings.opening)
                interleavings.opening(*walked / component);
            auto next = openBeneath(directory.get(), component, O_RDONLY | O_DIRECTORY);
            if (!next)
                return std::unexpected(next.error());
            directory = std::move(*next);
            *walked /= component;
        }
        if (interleavings.opening)
            interleavings.opening(*walked / components.back());
        // O_NONBLOCK keeps a FIFO from blocking the open until a writer appears.
        auto file = openBeneath(directory.get(), components.back(), O_RDONLY | O_NONBLOCK);
        if (!file)
            return std::unexpected(file.error());
        *walked /= components.back();
        return Walked { .directory = std::move(directory),
                        .file = std::move(*file),
                        .path = std::move(*walked) };
    }

    /// Unlinks @p name in @p directory, but only while the directory is still where @p path says -- not moved
    /// out of the roots since the walk -- and the name still names the file @p read was.
    void unlinkIfSame(int directory, std::filesystem::path const& path, struct stat const& read)
    {
        if (descriptorPath(directory) != path.parent_path())
            return;
        auto const name = path.filename();
        struct stat current {};
        if (::fstatat(directory, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
            && current.st_dev == read.st_dev && current.st_ino == read.st_ino)
            (void) ::unlinkat(directory, name.c_str(), 0);
    }
} // namespace

std::expected<std::vector<uint8_t>, MediaRefusal> PosixMediumSource::read(std::string_view path,
                                                                          Medium medium,
                                                                          MediaPolicy const& policy) const
{
    // Judged as requested, before the filesystem is touched at all.
    auto const requested = requestedPath(policy, medium, path);
    if (!requested)
        return std::unexpected(requested.error());

    if (_interleavings.pathAccepted)
        _interleavings.pathAccepted(requested->path);

    // Down from the root, following no link: nothing below it is resolved beyond what the path names.
    auto walked = walkBeneath(*requested, policy, medium, _interleavings);
    if (!walked)
        return std::unexpected(walked.error());
    auto const& canonical = walked->path;
    auto const& fd = walked->file;

    // Defence in depth: the descriptor must name what was walked.
    if (descriptorPath(fd.get()) != canonical)
        return std::unexpected(MediaRefusal::Unreadable);

    struct stat status {};
    if (::fstat(fd.get(), &status) != 0)
        return std::unexpected(MediaRefusal::Unreadable);
    if (auto const accepted = checkOpenedFile(policy, openedFileOf(status, _readerId)); !accepted)
        return std::unexpected(accepted.error());

    auto bytes = readBounded(static_cast<uint64_t>(status.st_size),
                             policy.sizeLimit,
                             [&fd](std::span<uint8_t> into) { return readSome(fd.get(), into); });
    if (!bytes)
        return bytes;

    if (deletionAfterRead(policy, canonical, medium) == DeletionAfterRead::Delete)
    {
        if (_interleavings.beforeDelete)
            _interleavings.beforeDelete(canonical);
        unlinkIfSame(walked->directory.get(), canonical, status);
    }
    return bytes;
}

} // namespace vtbackend::kitty_graphics
