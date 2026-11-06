// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverMemory.hpp>

#ifdef _WIN32

    #include <cstddef>
    #include <cstring>
    #include <limits>

    #include <Windows.h>

namespace vthost::handover
{

namespace
{
    /// The length of the image, ahead of it in the mapping: a mapping reports its size rounded up to a page.
    using LengthPrefix = uint64_t;

    /// @return @p fd as the handle it names (HandleToLong() made it).
    [[nodiscard]] HANDLE handleOf(int fd) noexcept
    {
        return LongToHandle(fd);
    }

    /// A view of a whole mapping, unmapped as it goes.
    class MappedView
    {
      public:
        MappedView(HANDLE mapping, DWORD access) noexcept:
            _address { MapViewOfFile(mapping, access, 0, 0, 0) }
        {
        }
        MappedView(MappedView const&) = delete;
        MappedView& operator=(MappedView const&) = delete;
        MappedView(MappedView&&) = delete;
        MappedView& operator=(MappedView&&) = delete;
        ~MappedView()
        {
            if (_address != nullptr)
                UnmapViewOfFile(_address);
        }

        [[nodiscard]] bool isMapped() const noexcept { return _address != nullptr; }
        [[nodiscard]] std::byte* data() const noexcept { return static_cast<std::byte*>(_address); }

        /// @return How many bytes the view spans: the mapping's size, rounded up to a page.
        [[nodiscard]] std::size_t size() const noexcept
        {
            auto region = MEMORY_BASIC_INFORMATION {};
            if (VirtualQuery(_address, &region, sizeof(region)) == 0)
                return 0;
            return region.RegionSize;
        }

      private:
        void* _address;
    };

    /// A pagefile-backed file mapping with no name: nothing on disk holds the image, and nothing but a handle
    /// reaches it -- the one the successor process inherits. The "descriptor" this interface speaks of is
    /// that handle, as HandleToLong() narrows it, which Windows guarantees loses nothing (handles stay within
    /// 32 bits so that 32- and 64-bit processes can share them).
    class MappedHandoverMemory final: public HandoverMemory
    {
      public:
        [[nodiscard]] std::expected<int, HandoverError> write(std::span<uint8_t const> image) override
        {
            auto const size = uint64_t { sizeof(LengthPrefix) } + image.size();
            // Not inheritable: only the launch that names it in its handle list takes it.
            auto* const mapping = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                                     nullptr,
                                                     PAGE_READWRITE,
                                                     static_cast<DWORD>(size >> 32),
                                                     static_cast<DWORD>(size & 0xFFFF'FFFFU),
                                                     nullptr);
            if (mapping == nullptr)
                return std::unexpected { HandoverError::MemoryUnavailable };
            {
                auto const view = MappedView { mapping, FILE_MAP_WRITE };
                if (!view.isMapped())
                {
                    CloseHandle(mapping);
                    return std::unexpected { HandoverError::MemoryUnavailable };
                }
                auto const length = LengthPrefix { image.size() };
                std::memcpy(view.data(), &length, sizeof(length));
                if (!image.empty())
                    std::memcpy(view.data() + sizeof(length), image.data(), image.size());
            }
            return HandleToLong(mapping);
        }

        [[nodiscard]] std::expected<std::vector<uint8_t>, HandoverError> read(int fd) override
        {
            auto const view = MappedView { handleOf(fd), FILE_MAP_READ };
            if (!view.isMapped())
                return std::unexpected { HandoverError::MemoryUnavailable };
            auto const spanned = view.size();
            auto length = LengthPrefix {};
            if (spanned < sizeof(length))
                return std::unexpected { HandoverError::Truncated };
            std::memcpy(&length, view.data(), sizeof(length));
            // A length the mapping does not hold, or one no image may have, is no image.
            if (length > MaxHandoverImageSize || length > spanned - sizeof(length))
                return std::unexpected { HandoverError::Truncated };
            auto bytes = std::vector<uint8_t>(static_cast<std::size_t>(length));
            if (!bytes.empty())
                std::memcpy(bytes.data(), view.data() + sizeof(length), bytes.size());
            return bytes;
        }

        /// Nothing: there is no exec, and the successor's launch makes inheritable what it names, for itself
        /// alone (@see SuccessorHandoff).
        [[nodiscard]] std::expected<void, HandoverError> keepAcrossExec(int /*fd*/) override { return {}; }

        /// Nothing, as keepAcrossExec().
        [[nodiscard]] std::expected<void, HandoverError> closeOnExecAgain(int /*fd*/) override { return {}; }

        void discard(int fd) override { CloseHandle(handleOf(fd)); }
    };
} // namespace

std::unique_ptr<HandoverMemory> makeNativeHandoverMemory()
{
    return std::make_unique<MappedHandoverMemory>();
}

std::unique_ptr<HandoverMemory> makeHandoverMemory(HandoverMemoryKind /*kind*/)
{
    // Windows has one kind of anonymous memory to hand over: an unnamed mapping.
    return std::make_unique<MappedHandoverMemory>();
}

} // namespace vthost::handover

#endif
