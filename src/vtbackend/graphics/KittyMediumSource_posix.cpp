// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyMediumSource.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <format>
#include <optional>
#include <ranges>
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

    /// Opens @p path one component at a time down from @p root, following no link below the root: a link's
    /// target is never looked up, so no component reaches an automount or a network filesystem, nor leaves
    /// the root. The root itself is configuration, and its own links (macOS `/tmp`) are followed.
    [[nodiscard]] std::expected<Walked, MediaRefusal> walkBeneath(std::filesystem::path const& root,
                                                                  std::filesystem::path const& path)
    {
        auto directory = FileDescriptor { ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
        if (directory.get() < 0)
            return std::unexpected(MediaRefusal::Unreadable);
        auto walked = descriptorPath(directory.get());
        if (!walked)
            return std::unexpected(MediaRefusal::Unreadable);

        auto components = std::vector<std::filesystem::path> {};
        for (auto const& component: path.lexically_relative(root))
            if (!component.empty())
                components.push_back(component);
        if (components.empty())
            return std::unexpected(MediaRefusal::NotRegularFile);

        for (auto const& component: components | std::views::take(components.size() - 1))
        {
            auto next = openBeneath(directory.get(), component, O_RDONLY | O_DIRECTORY);
            if (!next)
                return std::unexpected(next.error());
            directory = std::move(*next);
            *walked /= component;
        }
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
    // Refused before the filesystem is touched at all.
    if (auto const accepted = checkMedium(policy, medium); !accepted)
        return std::unexpected(accepted.error());
    if (path.empty() || path.contains('\0'))
        return std::unexpected(MediaRefusal::Unreadable);

    // Judged as requested before anything is looked up: a lookup outside the roots could reach an automount
    // or a network filesystem -- a stall under the terminal lock, and a beacon to whoever named it. A
    // relative path is refused too, as it would resolve against the terminal's own working directory.
    auto const requested = std::filesystem::path { path }.lexically_normal();
    if (auto const accepted = checkPath(policy, requested, medium); !accepted)
        return std::unexpected(accepted.error());
    auto const root = enclosingRoot(policy, requested, medium);
    if (!root)
        return std::unexpected(MediaRefusal::OutsideRoots);

    if (_interleavings.pathAccepted)
        _interleavings.pathAccepted(requested);

    // Down from the root, following no link: nothing below it is resolved beyond what the path names.
    auto walked = walkBeneath(*root, requested);
    if (!walked)
        return std::unexpected(walked.error());
    auto const& canonical = walked->path;
    // The real path, judged again: the root's own links may have led elsewhere than its spelling says.
    if (auto const accepted = checkPath(policy, canonical, medium); !accepted)
        return std::unexpected(accepted.error());
    auto const& fd = walked->file;

    // Defence in depth: the descriptor must name what was walked.
    if (descriptorPath(fd.get()) != canonical)
        return std::unexpected(MediaRefusal::Unreadable);

    struct stat status {};
    if (::fstat(fd.get(), &status) != 0)
        return std::unexpected(MediaRefusal::Unreadable);
    if (auto const accepted = checkOpenedFile(policy, openedFileOf(status)); !accepted)
        return std::unexpected(accepted.error());

    auto bytes = readBounded(fd.get(), static_cast<uint64_t>(status.st_size), policy.sizeLimit);
    if (!bytes)
        return bytes;

    if (deletionAfterRead(policy, canonical, medium) == DeletionAfterRead::Delete)
    {
        if (_interleavings.beforeUnlink)
            _interleavings.beforeUnlink(canonical);
        unlinkIfSame(walked->directory.get(), canonical, status);
    }
    return bytes;
}

} // namespace vtbackend::kitty_graphics
