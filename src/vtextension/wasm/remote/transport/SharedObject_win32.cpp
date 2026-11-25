// SPDX-License-Identifier: Apache-2.0
//
// Shared memory objects on Windows: an unnamed, pagefile-backed section, not inheritable until it is handed
// over. A section cannot be resized once created, so it needs no seal.
//
// A view's size is the section's, rounded up to the page; a mapping checks it against the size the object
// was made of, as POSIX checks fstat()'s.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

#include <vtextension/wasm/remote/transport/SharedObject.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// How an object for one use is made.
    struct UseRow
    {
        SharedObjectUse use; ///< The use.
        DWORD sectionKind;   ///< What backs its pages: committed at once (SEC_COMMIT).
    };

    /// Every use, at its enumerator's index.
    constexpr auto Uses = std::array {
        UseRow { .use = SharedObjectUse::GuestMemory, .sectionKind = SEC_COMMIT },
        UseRow { .use = SharedObjectUse::InterruptPage, .sectionKind = SEC_COMMIT },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, Uses.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(Uses[index].use) == index;
                                      }));

    /// @return How many bytes the view at @p base spans: every region that the one allocation it began
    ///         holds, committed or reserved alike; 0 if it cannot be asked.
    [[nodiscard]] uint64_t viewBytes(void* base) noexcept
    {
        auto total = uint64_t { 0 };
        auto region = MEMORY_BASIC_INFORMATION {};
        auto* cursor = static_cast<std::byte*>(base);
        while (::VirtualQuery(cursor, &region, sizeof(region)) != 0 && region.AllocationBase == base)
        {
            total += region.RegionSize;
            cursor += region.RegionSize;
        }
        return total;
    }
} // namespace

std::expected<OwnedHandle, SharedObjectError> createSharedObject(uint64_t bytes, SharedObjectUse use)
{
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max())
        return std::unexpected(SharedObjectError::WrongSize);
    auto object =
        OwnedHandle { ::CreateFileMappingW(INVALID_HANDLE_VALUE,
                                           nullptr,
                                           PAGE_READWRITE | Uses[std::to_underlying(use)].sectionKind,
                                           static_cast<DWORD>(bytes >> 32),
                                           static_cast<DWORD>(bytes & 0xFFFF'FFFF),
                                           nullptr) };
    if (!object)
        return std::unexpected(SharedObjectError::Io);
    return object;
}

std::expected<void, SharedObjectError> sealSharedObject([[maybe_unused]] NativeHandle handle,
                                                        [[maybe_unused]] SharedObjectUse use) noexcept
{
    // A section's size is fixed at creation; there is nothing to seal.
    return {};
}

std::expected<SharedMapping, SharedObjectError> SharedMapping::map(NativeHandle handle,
                                                                   uint64_t bytes,
                                                                   MapAccess access)
{
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max())
        return std::unexpected(SharedObjectError::WrongSize);
    // The whole section, so that its size shows; only the first bytes are handed out.
    auto const viewAccess = access == MapAccess::ReadWrite ? FILE_MAP_READ | FILE_MAP_WRITE : FILE_MAP_READ;
    auto* const view = ::MapViewOfFile(handle, viewAccess, 0, 0, 0);
    if (view == nullptr)
        return std::unexpected(SharedObjectError::Io);
    auto const extent = viewBytes(view);
    if (extent == 0)
    {
        ::UnmapViewOfFile(view);
        return std::unexpected(SharedObjectError::Io);
    }
    if (!fitsSharedObject(extent, bytes))
    {
        ::UnmapViewOfFile(view);
        return std::unexpected(SharedObjectError::WrongSize);
    }
    return SharedMapping { std::span { static_cast<std::byte*>(view), static_cast<std::size_t>(bytes) },
                           access };
}

void SharedMapping::release() noexcept
{
    if (!_bytes.empty())
        ::UnmapViewOfFile(_bytes.data());
    _bytes = {};
}

} // namespace vtextension::wasm::remote::transport
