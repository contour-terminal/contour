// SPDX-License-Identifier: Apache-2.0
//
// Guest memory on POSIX: an inaccessible reservation of the object and its guard (PROT_NONE, MAP_NORESERVE,
// so that it charges nothing), the object mapped shared over its start (MAP_FIXED), writable to prove the
// handle grants writing, and every byte past the open ones closed again. Opening more is one mprotect().
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <expected>

#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// @return The error a failed size check of the object maps to.
    [[nodiscard]] MappingError mappingErrorOf(SharedObjectError error) noexcept
    {
        return error == SharedObjectError::WrongSize ? MappingError::WrongSize : MappingError::Io;
    }
} // namespace

std::expected<GuestMemoryMapping, MappingError> GuestMemoryMapping::map(NativeHandle object,
                                                                        uint64_t objectBytes,
                                                                        uint64_t initialBytes,
                                                                        uint64_t guardBytes)
{
    if (!isGuestMemoryLayout(objectBytes, initialBytes, guardBytes))
        return std::unexpected(MappingError::WrongSize);
    if (auto const sized = checkSharedObjectSize(object, objectBytes); !sized)
        return std::unexpected(mappingErrorOf(sized.error()));

    auto const objectSize = static_cast<std::size_t>(objectBytes);
    auto const reservationSize = static_cast<std::size_t>(objectBytes + guardBytes);
    auto* const reservation =
        ::mmap(nullptr, reservationSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED)
        return std::unexpected(MappingError::Io);
    // From here on the mapping owns the reservation, and unmaps it on every way out.
    auto mapping = GuestMemoryMapping { static_cast<std::byte*>(reservation), objectBytes, 0, guardBytes };
    if (::mmap(reservation, objectSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, object, 0)
        == MAP_FAILED)
        return std::unexpected(MappingError::Io);
    // Writable it proved the handle grants writing; only the initial bytes stay so.
    auto const closed = objectSize - static_cast<std::size_t>(initialBytes);
    if (closed != 0 && ::mprotect(mapping._base + initialBytes, closed, PROT_NONE) != 0)
        return std::unexpected(MappingError::Io);
    mapping._accessibleBytes = initialBytes;
    return mapping;
}

std::expected<void, MappingError> GuestMemoryMapping::openRange(uint64_t from, uint64_t to) noexcept
{
    if (::mprotect(_base + from, static_cast<std::size_t>(to - from), PROT_READ | PROT_WRITE) != 0)
        return std::unexpected(MappingError::Io);
    return {};
}

void GuestMemoryMapping::release() noexcept
{
    // One munmap() covers the object's mapping and the guard's reservation alike.
    if (_base != nullptr)
        ::munmap(_base, static_cast<std::size_t>(_objectBytes + _guardBytes));
    _base = nullptr;
}

bool canMapGuestMemory() noexcept
{
    return true;
}

} // namespace vtextension::wasm::remote::transport
