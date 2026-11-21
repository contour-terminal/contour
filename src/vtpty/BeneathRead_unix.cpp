// SPDX-License-Identifier: Apache-2.0
#include <vtpty/BeneathRead.hpp>
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/OwnedFd.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__) && __has_include(<linux/openat2.h>)
    #include <sys/syscall.h>

    #include <linux/openat2.h>
    #ifdef SYS_openat2
        #define VTPTY_HAVE_OPENAT2 1
    #endif
#endif

namespace vtpty
{

namespace
{
    /// Which step of the walk failed.
    enum class Step : uint8_t
    {
        Directory, ///< A directory above the file.
        File,      ///< The file itself, looked at or opened.
    };

    /// What an errno from one step of the walk means.
    struct ErrnoRow
    {
        Step step;
        int error;
        BeneathReadError meaning;
    };

    /// Every errno the walk answers by name; any other is `Unreadable`, never `DirectoryMissing`.
    // clang-format off
    constexpr auto ErrnoRows = std::array {
        ErrnoRow { Step::Directory, ENOENT,  BeneathReadError::DirectoryMissing },
        ErrnoRow { Step::Directory, ENOTDIR, BeneathReadError::NotRegular },
        ErrnoRow { Step::Directory, ELOOP,   BeneathReadError::Escapes },  // A link, met with O_NOFOLLOW.
        ErrnoRow { Step::Directory, EMLINK,  BeneathReadError::Escapes },  // FreeBSD's answer to the same.
        ErrnoRow { Step::Directory, EXDEV,   BeneathReadError::Escapes },  // openat2 met a mount.
        ErrnoRow { Step::Directory, EACCES,  BeneathReadError::Unreadable },
        ErrnoRow { Step::File,      ENOENT,  BeneathReadError::NotRegular }, // A missing file, its directory there.
        ErrnoRow { Step::File,      ENOTDIR, BeneathReadError::NotRegular },
        ErrnoRow { Step::File,      ENXIO,   BeneathReadError::NotRegular }, // A socket, or a device with none behind it.
        ErrnoRow { Step::File,      ELOOP,   BeneathReadError::Escapes },
        ErrnoRow { Step::File,      EMLINK,  BeneathReadError::Escapes },
        ErrnoRow { Step::File,      EXDEV,   BeneathReadError::Escapes },
        ErrnoRow { Step::File,      EACCES,  BeneathReadError::Unreadable },
    };
    // clang-format on

    /// @return What @p error at @p step means.
    [[nodiscard]] BeneathReadError meaningOf(Step step, int error) noexcept
    {
        auto const row = std::ranges::find_if(ErrnoRows, [&](ErrnoRow const& candidate) {
            return candidate.step == step && candidate.error == error;
        });
        return row == ErrnoRows.end() ? BeneathReadError::Unreadable : row->meaning;
    }

    /// @return Whether @p component names one entry of its directory, and no other.
    [[nodiscard]] bool isPlainComponent(std::string_view component) noexcept
    {
        return !component.empty() && component != "." && component != ".." && !component.contains('/')
               && !component.contains('\0');
    }

    /// @return When @p status says it was modified, in milliseconds since the epoch; 0 before it.
    [[nodiscard]] uint64_t modifiedUnixMsOf(struct stat const& status) noexcept
    {
#ifdef __APPLE__
        auto const& modified = status.st_mtimespec;
#else
        auto const& modified = status.st_mtim;
#endif
        if (modified.tv_sec < 0)
            return 0;
        return (static_cast<uint64_t>(modified.tv_sec) * 1000U)
               + (static_cast<uint64_t>(modified.tv_nsec) / 1'000'000U);
    }

    /// @return What @p mode says the entry is.
    [[nodiscard]] FileKind kindOf(mode_t mode) noexcept
    {
        if (S_ISREG(mode))
            return FileKind::Regular;
        if (S_ISDIR(mode))
            return FileKind::Directory;
        if (S_ISLNK(mode))
            return FileKind::Symlink;
        return FileKind::Other;
    }

    /// @return @p status as a read beneath a directory needs it.
    [[nodiscard]] FileStatus statusOf(struct stat const& status) noexcept
    {
        return FileStatus { .kind = kindOf(status.st_mode),
                            .identity = FileIdentity { .device = static_cast<uint64_t>(status.st_dev),
                                                       .inode = static_cast<uint64_t>(status.st_ino) },
                            .links = static_cast<uint64_t>(status.st_nlink),
                            .sizeBytes = status.st_size < 0 ? 0 : static_cast<uint64_t>(status.st_size),
                            .modifiedUnixMs = modifiedUnixMsOf(status) };
    }

    /// @return The `open` flags of @p target.
    [[nodiscard]] constexpr int flagsOf(BeneathTarget target) noexcept
    {
        switch (target)
        {
            case BeneathTarget::Directory: return O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
            case BeneathTarget::File: return O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
        }
        return O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
    }

    /// Runs @p call until it is not interrupted by a signal.
    template <typename Call>
    [[nodiscard]] int retried(Call call) noexcept
    {
        while (true)
        {
            auto const result = call();
            if (result >= 0 || errno != EINTR)
                return result;
        }
    }

    /// The system calls of this machine.
    class NativeBeneathSystem final: public IBeneathSystem
    {
      public:
        explicit NativeBeneathSystem(BeneathResolution resolution) noexcept:
            _kernelResolves { resolution == BeneathResolution::Kernel }
        {
        }

        [[nodiscard]] std::expected<OwnedFd, int> open(int directory,
                                                       std::string const& name,
                                                       BeneathTarget target) override
        {
            auto const flags = flagsOf(target);
            auto descriptor = kernelOpen(directory, name, flags);
            if (!descriptor)
                descriptor = retried([&] { return ::openat(directory, name.c_str(), flags); });
            if (*descriptor < 0)
                return std::unexpected(errno);
            return OwnedFd { *descriptor };
        }

        [[nodiscard]] std::expected<FileStatus, int> statusAt(int directory, std::string const& name) override
        {
#ifdef VTPTY_HAVE_OPENAT2
            // An O_PATH descriptor is a look, not an open: the file's own open() (a device's) never runs,
            // and the kernel refuses a link or a mount point before anything is asked of it.
            if (auto const looked = kernelOpen(directory, name, O_PATH | O_NOFOLLOW | O_CLOEXEC))
            {
                if (*looked < 0)
                    return std::unexpected(errno);
                auto const descriptor = OwnedFd { *looked };
                return status(descriptor.get());
            }
#endif
            struct stat status {};
            if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
                return std::unexpected(errno);
            return statusOf(status);
        }

        [[nodiscard]] std::expected<FileStatus, int> status(int descriptor) override
        {
            struct stat status {};
            if (::fstat(descriptor, &status) != 0)
                return std::unexpected(errno);
            return statusOf(status);
        }

      private:
        /// Opens @p name beneath @p directory with `openat2`, confined to it.
        /// @return The descriptor, or -1 with errno set; std::nullopt where the kernel cannot (the caller
        /// then
        ///         resolves it per component).
        [[nodiscard]] std::optional<int> kernelOpen(int directory, std::string const& name, int flags)
        {
#ifdef VTPTY_HAVE_OPENAT2
            if (!_kernelResolves.load(std::memory_order_relaxed))
                return std::nullopt;
            auto how = open_how {};
            how.flags = static_cast<uint64_t>(flags);
            how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV | RESOLVE_NO_MAGICLINKS;
            auto const descriptor = retried([&] {
                return static_cast<int>(::syscall(SYS_openat2, directory, name.c_str(), &how, sizeof(how)));
            });
            if (descriptor < 0 && errno == ENOSYS)
            {
                // A kernel before 5.6, or a seccomp filter that knows no openat2: never asked again.
                _kernelResolves.store(false, std::memory_order_relaxed);
                return std::nullopt;
            }
            return descriptor;
#else
            (void) directory;
            (void) name;
            (void) flags;
            return std::nullopt;
#endif
        }

        std::atomic<bool> _kernelResolves;
    };

    class NativeBeneathReader final: public IBeneathReader
    {
      public:
        explicit NativeBeneathReader(IBeneathSystem& system) noexcept: _system { system } {}

        [[nodiscard]] std::expected<BeneathFile, BeneathReadError> read(
            DirectoryHandle const& root,
            std::span<std::string_view const> components,
            std::size_t maxBytes,
            std::span<FileIdentity const> fenced) override
        {
            if (components.empty() || !std::ranges::all_of(components, isPlainComponent))
                return std::unexpected(BeneathReadError::Escapes);
            auto const rootDevice = deviceOfRoot(root);
            if (!rootDevice)
                return std::unexpected(rootDevice.error());
            auto const walked = walk(root, *rootDevice, components.first(components.size() - 1), fenced);
            if (!walked)
                return std::unexpected(walked.error());
            auto const directory = walked->isOpen() ? walked->get() : root.descriptor();

            auto const name = std::string { components.back() };
            // Never open what is no regular file, where a look can tell: opening a device may do something.
            // The descriptor's own fstat below stays the authority.
            auto const looked = _system.statusAt(directory, name);
            if (!looked)
                return std::unexpected(meaningOf(Step::File, looked.error()));
            if (looked->kind == FileKind::Symlink)
                return std::unexpected(BeneathReadError::Escapes);
            if (looked->kind != FileKind::Regular)
                return std::unexpected(BeneathReadError::NotRegular);

            auto const file = _system.open(directory, name, BeneathTarget::File);
            if (!file)
                return std::unexpected(openFailure(Step::File, directory, name, file.error()));
            auto const status = _system.status(file->get());
            if (!status)
                return std::unexpected(BeneathReadError::Unreadable);
            if (status->kind != FileKind::Regular)
                return std::unexpected(BeneathReadError::NotRegular);
            // A second name may lie outside the root; a mount brings another filesystem's tree in.
            if (status->links != 1 || status->identity.device != *rootDevice)
                return std::unexpected(BeneathReadError::Escapes);
            return readUpTo(file->get(), status->sizeBytes, maxBytes).transform([&](std::string bytes) {
                return BeneathFile { .bytes = std::move(bytes), .modifiedUnixMs = status->modifiedUnixMs };
            });
        }

        [[nodiscard]] std::expected<FileIdentity, BeneathReadError> identityOf(
            DirectoryHandle const& root, std::span<std::string_view const> components) override
        {
            if (!std::ranges::all_of(components, isPlainComponent))
                return std::unexpected(BeneathReadError::Escapes);
            auto const rootDevice = deviceOfRoot(root);
            if (!rootDevice)
                return std::unexpected(rootDevice.error());
            auto const walked = walk(root, *rootDevice, components, {});
            if (!walked)
                return std::unexpected(walked.error());
            return _system.status(walked->isOpen() ? walked->get() : root.descriptor())
                .transform([](FileStatus const& status) { return status.identity; })
                .transform_error([](int /*error*/) { return BeneathReadError::Unreadable; });
        }

      private:
        /// @return The device @p root lives on; `DirectoryMissing` once it was removed while held.
        [[nodiscard]] std::expected<uint64_t, BeneathReadError> deviceOfRoot(DirectoryHandle const& root)
        {
            auto const status = _system.status(root.descriptor());
            if (!status)
                return std::unexpected(BeneathReadError::Unreadable);
            // A root removed while it was held: what lay beneath it is gone with it.
            if (status->links == 0)
                return std::unexpected(BeneathReadError::DirectoryMissing);
            return status->identity.device;
        }

        /// Opens @p directories beneath @p root, each by the one above it: no name is ever resolved from the
        /// root's path. A directory on another device than @p rootDevice is `Escapes`, one of @p fenced
        /// `Fenced`.
        /// @return The last directory opened; an empty descriptor when @p directories is empty (the root).
        [[nodiscard]] std::expected<OwnedFd, BeneathReadError> walk(
            DirectoryHandle const& root,
            uint64_t rootDevice,
            std::span<std::string_view const> directories,
            std::span<FileIdentity const> fenced)
        {
            auto held = OwnedFd {};
            for (auto const component: directories)
            {
                auto const directory = held.isOpen() ? held.get() : root.descriptor();
                auto const name = std::string { component };
                auto next = _system.open(directory, name, BeneathTarget::Directory);
                if (!next)
                    return std::unexpected(openFailure(Step::Directory, directory, name, next.error()));
                auto const status = _system.status(next->get());
                if (!status)
                    return std::unexpected(BeneathReadError::Unreadable);
                if (status->identity.device != rootDevice)
                    return std::unexpected(BeneathReadError::Escapes);
                // Another root's directory, however the path spells it (a case-insensitive name, say).
                if (std::ranges::contains(fenced, status->identity))
                    return std::unexpected(BeneathReadError::Fenced);
                held = std::move(*next);
            }
            return held;
        }

        /// @return Why the open of @p name beneath @p directory at @p step failed with @p error.
        [[nodiscard]] BeneathReadError openFailure(Step step,
                                                   int directory,
                                                   std::string const& name,
                                                   int error)
        {
            // Some systems answer O_DIRECTORY before O_NOFOLLOW: a link is a link, whatever it points at.
            if (error == ENOTDIR)
                if (auto const looked = _system.statusAt(directory, name);
                    looked && looked->kind == FileKind::Symlink)
                    return BeneathReadError::Escapes;
            return meaningOf(step, error);
        }

        /// Reads at most @p maxBytes + 1 bytes of @p descriptor: the bound is what was read.
        /// @param sizeHint What fstat said the size was: how much is reserved, never how much is read.
        [[nodiscard]] static std::expected<std::string, BeneathReadError> readUpTo(int descriptor,
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
                auto const got = ::read(descriptor, bytes.data() + have, want);
                if (got < 0 && errno == EINTR)
                {
                    bytes.resize(have);
                    continue;
                }
                if (got < 0)
                    return std::unexpected(BeneathReadError::Unreadable);
                bytes.resize(have + static_cast<std::size_t>(got));
                if (got == 0)
                    break;
            }
            if (bytes.size() > maxBytes)
                return std::unexpected(BeneathReadError::TooLarge);
            return bytes;
        }

        IBeneathSystem& _system;
    };
} // namespace

std::unique_ptr<IBeneathSystem> makeBeneathSystem(BeneathResolution resolution)
{
    return std::make_unique<NativeBeneathSystem>(resolution);
}

IBeneathSystem& nativeBeneathSystem() noexcept
{
    static auto instance = NativeBeneathSystem { BeneathResolution::Kernel };
    return instance;
}

std::unique_ptr<IBeneathReader> makeBeneathReader(IBeneathSystem& system)
{
    return std::make_unique<NativeBeneathReader>(system);
}

IBeneathReader& nativeBeneathReader() noexcept
{
    static auto instance = NativeBeneathReader { nativeBeneathSystem() };
    return instance;
}

} // namespace vtpty
