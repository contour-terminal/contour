// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverMemory.hpp>

#ifndef _WIN32

    #include <vtpty/OwnedFd.hpp>
    #include <vtpty/UnixUtils.hpp>

    #include <sys/mman.h>
    #include <sys/stat.h>

    #include <cerrno>
    #include <cstring>
    #include <format>
    #include <limits>
    #include <new>
    #include <random>
    #include <ranges>
    #include <span>
    #include <string>
    #include <string_view>
    #include <utility>

    #include <fcntl.h>
    #include <unistd.h>

namespace vthost::handover
{

namespace
{
    /// Sets or clears @p fd's close-on-exec flag.
    [[nodiscard]] std::expected<void, HandoverError> setInheritance(
        int fd, vtpty::util::ExecInheritance inheritance) noexcept
    {
        return vtpty::util::setExecInheritance(fd, inheritance).transform_error([](std::error_code) {
            return HandoverError::MemoryUnavailable;
        });
    }

    /// A shared mapping of a whole memory object, unmapped on scope exit.
    class Mapping
    {
      public:
        /// @param fd The memory object.
        /// @param length How much of it to map, from its start; not zero.
        /// @param protection PROT_READ, or PROT_READ | PROT_WRITE.
        Mapping(int fd, size_t length, int protection) noexcept:
            _address { ::mmap(nullptr, length, protection, MAP_SHARED, fd, 0) }, _length { length }
        {
        }
        Mapping(Mapping const&) = delete;
        Mapping& operator=(Mapping const&) = delete;
        Mapping(Mapping&&) = delete;
        Mapping& operator=(Mapping&&) = delete;
        ~Mapping()
        {
            if (_address != MAP_FAILED)
                ::munmap(_address, _length);
        }

        /// @return Whether the object is mapped.
        [[nodiscard]] bool isMapped() const noexcept { return _address != MAP_FAILED; }

        /// @return The mapped bytes.
        [[nodiscard]] std::span<uint8_t> bytes() const noexcept
        {
            return { static_cast<uint8_t*>(_address), _length };
        }

      private:
        void* _address;
        size_t _length;
    };

    /// How a mapped memory object starts: the size of the image that follows it.
    using MappedHeader = uint64_t;

    /// The longest name Darwin gives a POSIX shared memory object (PSHMNAMLEN), the leading slash included.
    constexpr auto MaxSharedMemoryNameLength = size_t { 31 };

    /// @return A close-on-exec memfd, or -1 where the platform has none.
    int createMemFd()
    {
    #if defined(__linux__) || defined(__FreeBSD__)
        return ::memfd_create("contour-handover", MFD_CLOEXEC);
    #else
        errno = ENOSYS;
        return -1;
    #endif
    }

    /// @return A close-on-exec descriptor of an unlinked POSIX shared memory object, or -1.
    int createSharedMemory()
    {
        // No name survives this call: the object is created exclusively under a random one and unlinked at
        // once. The name fits Darwin's limit: "/ctr-ho-" and sixteen hex digits are 24 characters.
        auto device = std::random_device {};
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 8))
        {
            auto const random = (uint64_t { device() } << 32U) | uint64_t { device() };
            auto const name = std::format("/ctr-ho-{:016x}", random);
            static_assert(std::string_view { "/ctr-ho-" }.size() + 16 <= MaxSharedMemoryNameLength);
            auto const fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (fd < 0)
            {
                if (errno == EEXIST)
                    continue;
                return -1;
            }
            ::shm_unlink(name.c_str());
            if (!vtpty::util::setCloseOnExec(fd))
            {
                ::close(fd);
                return -1;
            }
            return fd;
        }
        return -1;
    }

    /// Writes @p image through the descriptor, sizing the object to it first.
    [[nodiscard]] std::expected<void, HandoverError> writeThroughDescriptor(int fd,
                                                                            std::span<uint8_t const> image)
    {
        if (::ftruncate(fd, static_cast<off_t>(image.size())) != 0)
            return std::unexpected { HandoverError::MemoryUnavailable };
        auto written = size_t { 0 };
        while (written < image.size())
        {
            auto const count =
                ::pwrite(fd, image.data() + written, image.size() - written, static_cast<off_t>(written));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return std::unexpected { HandoverError::MemoryUnavailable };
            written += static_cast<size_t>(count);
        }
        return {};
    }

    /// Reads all of an object of @p size bytes through its descriptor.
    [[nodiscard]] std::expected<std::vector<uint8_t>, HandoverError> readThroughDescriptor(int fd,
                                                                                           size_t size)
    {
        auto bytes = std::vector<uint8_t>(size);
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

    /// Writes @p image through a shared mapping, its size ahead of it, sizing the object once.
    [[nodiscard]] std::expected<void, HandoverError> writeThroughMapping(int fd,
                                                                         std::span<uint8_t const> image)
    {
        auto const total = sizeof(MappedHeader) + image.size();
        if (::ftruncate(fd, static_cast<off_t>(total)) != 0)
            return std::unexpected { HandoverError::MemoryUnavailable };
        auto const mapping = Mapping { fd, total, PROT_READ | PROT_WRITE };
        if (!mapping.isMapped())
            return std::unexpected { HandoverError::MemoryUnavailable };
        auto const header = MappedHeader { image.size() };
        auto const bytes = mapping.bytes();
        std::memcpy(bytes.data(), &header, sizeof(header));
        if (!image.empty())
            std::memcpy(bytes.subspan(sizeof(header)).data(), image.data(), image.size());
        return {};
    }

    /// Reads the image a shared mapping of an object of @p size bytes holds after its size, which may be
    /// rounded up to a page.
    [[nodiscard]] std::expected<std::vector<uint8_t>, HandoverError> readThroughMapping(int fd, size_t size)
    {
        if (size < sizeof(MappedHeader))
            return std::unexpected { HandoverError::MemoryUnavailable };
        auto const mapping = Mapping { fd, size, PROT_READ };
        if (!mapping.isMapped())
            return std::unexpected { HandoverError::MemoryUnavailable };
        auto const bytes = mapping.bytes();
        auto header = MappedHeader {};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header > size - sizeof(MappedHeader) || header > MaxHandoverImageSize)
            return std::unexpected { HandoverError::MemoryUnavailable };
        auto const image = bytes.subspan(sizeof(MappedHeader), static_cast<size_t>(header));
        return std::vector<uint8_t>(image.begin(), image.end());
    }

    class NativeHandoverMemory final: public HandoverMemory
    {
      public:
        explicit NativeHandoverMemory(HandoverMemoryKind kind) noexcept: _kind { kind } {}

        [[nodiscard]] std::expected<int, HandoverError> write(std::span<uint8_t const> image) override
        {
            if (image.size() > MaxHandoverImageSize)
                return std::unexpected { HandoverError::MemoryUnavailable };
            auto owner = vtpty::OwnedFd { create() };
            if (!owner.isOpen())
                return std::unexpected { HandoverError::MemoryUnavailable };
            auto const written = [&] {
                switch (_kind.access)
                {
                    case HandoverMemoryAccess::Descriptor: return writeThroughDescriptor(owner.get(), image);
                    case HandoverMemoryAccess::Mapped: break;
                }
                return writeThroughMapping(owner.get(), image);
            }();
            if (!written)
                return std::unexpected { written.error() };
            return owner.release();
        }

        [[nodiscard]] std::expected<std::vector<uint8_t>, HandoverError> read(int fd) override
        {
            struct stat info {};
            // A mapped object carries its size ahead of the image, and may be rounded up to a page past it.
            auto const ceiling = MaxHandoverImageSize + sizeof(MappedHeader) + MaxPageSize;
            if (::fstat(fd, &info) != 0 || info.st_size < 0 || std::cmp_greater(info.st_size, ceiling)
                || std::cmp_greater(info.st_size, std::numeric_limits<size_t>::max()))
                return std::unexpected { HandoverError::MemoryUnavailable };
            auto const size = static_cast<size_t>(info.st_size);
            try
            {
                switch (_kind.access)
                {
                    case HandoverMemoryAccess::Descriptor:
                        if (std::cmp_greater(size, MaxHandoverImageSize))
                            return std::unexpected { HandoverError::MemoryUnavailable };
                        return readThroughDescriptor(fd, size);
                    case HandoverMemoryAccess::Mapped: break;
                }
                return readThroughMapping(fd, size);
            }
            catch (std::bad_alloc const&)
            {
                return std::unexpected { HandoverError::MemoryUnavailable };
            }
        }

        [[nodiscard]] std::expected<void, HandoverError> keepAcrossExec(int fd) override
        {
            return setInheritance(fd, vtpty::util::ExecInheritance::KeepOnExec);
        }

        [[nodiscard]] std::expected<void, HandoverError> closeOnExecAgain(int fd) override
        {
            return setInheritance(fd, vtpty::util::ExecInheritance::CloseOnExec);
        }

        void discard(int fd) override { ::close(fd); }

      private:
        /// The largest page any platform rounds a shared memory object's size up to.
        static constexpr auto MaxPageSize = uint64_t { 64 } * 1024;

        /// @return A close-on-exec descriptor of a new, empty memory object of this kind, or -1.
        [[nodiscard]] int create() const
        {
            switch (_kind.object)
            {
                case HandoverMemoryObject::MemFd: return createMemFd();
                case HandoverMemoryObject::SharedMemory: break;
            }
            return createSharedMemory();
        }

        HandoverMemoryKind _kind;
    };
} // namespace

std::unique_ptr<HandoverMemory> makeNativeHandoverMemory()
{
    return makeHandoverMemory(nativeHandoverMemoryKind());
}

std::unique_ptr<HandoverMemory> makeHandoverMemory(HandoverMemoryKind kind)
{
    return std::make_unique<NativeHandoverMemory>(kind);
}

} // namespace vthost::handover

#endif
