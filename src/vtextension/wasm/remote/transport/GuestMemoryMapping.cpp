// SPDX-License-Identifier: Apache-2.0
//
// What every system's guest memory mapping shares: placing the object and opening its pages are each system's
// own (GuestMemoryMapping_posix.cpp, GuestMemoryMapping_win32.cpp); the layout rules and owning the mapping
// are not.
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::remote::transport
{

bool isGuestMemoryLayout(uint64_t objectBytes, uint64_t initialBytes, uint64_t guardBytes) noexcept
{
    constexpr auto Page = proto::WasmPageBytes;
    constexpr auto Addressable = uint64_t { std::numeric_limits<std::size_t>::max() };
    return objectBytes != 0 && objectBytes % Page == 0 && initialBytes % Page == 0 && guardBytes % Page == 0
           && initialBytes <= objectBytes && objectBytes <= Addressable
           && guardBytes <= Addressable - objectBytes;
}

GuestMemoryMapping::GuestMemoryMapping(std::byte* base,
                                       uint64_t objectBytes,
                                       uint64_t accessibleBytes,
                                       uint64_t guardBytes) noexcept:
    _base { base },
    _objectBytes { objectBytes },
    _accessibleBytes { accessibleBytes },
    _guardBytes { guardBytes }
{
}

GuestMemoryMapping::GuestMemoryMapping(GuestMemoryMapping&& other) noexcept:
    _base { std::exchange(other._base, nullptr) },
    _objectBytes { std::exchange(other._objectBytes, 0) },
    _accessibleBytes { std::exchange(other._accessibleBytes, 0) },
    _guardBytes { std::exchange(other._guardBytes, 0) }
{
}

GuestMemoryMapping& GuestMemoryMapping::operator=(GuestMemoryMapping&& other) noexcept
{
    if (this != &other)
    {
        release();
        _base = std::exchange(other._base, nullptr);
        _objectBytes = std::exchange(other._objectBytes, 0);
        _accessibleBytes = std::exchange(other._accessibleBytes, 0);
        _guardBytes = std::exchange(other._guardBytes, 0);
    }
    return *this;
}

GuestMemoryMapping::~GuestMemoryMapping()
{
    release();
}

std::expected<void, MappingError> GuestMemoryMapping::open(uint64_t bytes) noexcept
{
    if (bytes > _objectBytes || bytes % proto::WasmPageBytes != 0)
        return std::unexpected(MappingError::WrongSize);
    if (bytes <= _accessibleBytes)
        return {};
    return openRange(_accessibleBytes, bytes).transform([&] { _accessibleBytes = bytes; });
}

} // namespace vtextension::wasm::remote::transport
