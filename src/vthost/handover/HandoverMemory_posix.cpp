// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverMemory.hpp>

#ifndef _WIN32

    #include <sys/mman.h>
    #include <sys/stat.h>

    #include <cerrno>
    #include <format>
    #include <new>
    #include <random>
    #include <ranges>
    #include <string>
    #include <utility>

    #include <fcntl.h>
    #include <unistd.h>

namespace vthost::handover
{

namespace
{
    /// Closes a descriptor on scope exit unless released.
    class FdGuard
    {
      public:
        explicit FdGuard(int fd) noexcept: _fd { fd } {}
        FdGuard(FdGuard const&) = delete;
        FdGuard& operator=(FdGuard const&) = delete;
        FdGuard(FdGuard&&) = delete;
        FdGuard& operator=(FdGuard&&) = delete;
        ~FdGuard()
        {
            if (_fd >= 0)
                ::close(_fd);
        }

        /// @return The descriptor, still owned by this guard.
        [[nodiscard]] int get() const noexcept { return _fd; }

        /// @return The descriptor, no longer closed by this guard.
        [[nodiscard]] int release() noexcept { return std::exchange(_fd, -1); }

      private:
        int _fd;
    };

    /// @return A close-on-exec descriptor of an anonymous memory object, or -1.
    int createAnonymousMemory()
    {
    #ifdef __linux__
        return ::memfd_create("contour-handover", MFD_CLOEXEC);
    #else
        // No name survives this call: the object is created exclusively under a random one and unlinked at
        // once.
        auto device = std::random_device {};
        for (auto const attempt: std::views::iota(0, 8))
        {
            (void) attempt;
            auto const name = std::format("/contour-handover-{:08x}{:08x}", device(), device());
            auto const fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (fd < 0)
            {
                if (errno == EEXIST)
                    continue;
                return -1;
            }
            ::shm_unlink(name.c_str());
            if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
            {
                ::close(fd);
                return -1;
            }
            return fd;
        }
        return -1;
    #endif
    }

    class NativeHandoverMemory final: public HandoverMemory
    {
      public:
        [[nodiscard]] std::expected<int, HandoverError> write(std::span<uint8_t const> image) override
        {
            auto owner = FdGuard { createAnonymousMemory() };
            auto const handle = owner.get();
            if (handle < 0)
                return std::unexpected { HandoverError::MemoryUnavailable };
            if (::ftruncate(handle, static_cast<off_t>(image.size())) != 0)
                return std::unexpected { HandoverError::MemoryUnavailable };
            auto written = size_t { 0 };
            while (written < image.size())
            {
                auto const count = ::pwrite(
                    handle, image.data() + written, image.size() - written, static_cast<off_t>(written));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return std::unexpected { HandoverError::MemoryUnavailable };
                written += static_cast<size_t>(count);
            }
            return owner.release();
        }

        [[nodiscard]] std::expected<std::vector<uint8_t>, HandoverError> read(int fd) override
        {
            struct stat info {};
            if (::fstat(fd, &info) != 0 || info.st_size < 0
                || std::cmp_greater(info.st_size, MaxHandoverMemorySize))
                return std::unexpected { HandoverError::MemoryUnavailable };
            try
            {
                auto bytes = std::vector<uint8_t>(static_cast<size_t>(info.st_size));
                auto done = size_t { 0 };
                while (done < bytes.size())
                {
                    auto const count =
                        ::pread(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(done));
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count <= 0)
                        return std::unexpected { HandoverError::MemoryUnavailable };
                    done += static_cast<size_t>(count);
                }
                return bytes;
            }
            catch (std::bad_alloc const&)
            {
                return std::unexpected { HandoverError::MemoryUnavailable };
            }
        }

        [[nodiscard]] std::expected<void, HandoverError> keepAcrossExec(int fd) override
        {
            auto const flags = ::fcntl(fd, F_GETFD);
            if (flags < 0 || ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) != 0)
                return std::unexpected { HandoverError::MemoryUnavailable };
            return {};
        }
    };
} // namespace

std::unique_ptr<HandoverMemory> makeNativeHandoverMemory()
{
    return std::make_unique<NativeHandoverMemory>();
}

} // namespace vthost::handover

#endif
