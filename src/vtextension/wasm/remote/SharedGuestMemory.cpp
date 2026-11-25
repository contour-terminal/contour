// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/SharedGuestMemory.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

namespace vtextension::wasm::remote
{

using transport::MapAccess;
using transport::OwnedHandle;
using transport::SharedMapping;
using transport::SharedObjectError;
using transport::SharedObjectUse;

namespace
{
    /// @return What a failure to make, seal or map the object is to the instance being made.
    [[nodiscard]] constexpr ExtensionError errorOf(SharedObjectError error) noexcept
    {
        switch (error)
        {
            case SharedObjectError::WrongSize: return ExtensionError::OutOfMemory;
            case SharedObjectError::Unsupported:
            case SharedObjectError::Io:
            case SharedObjectError::OutOfRange: return ExtensionError::RuntimeUnavailable;
        }
        return ExtensionError::RuntimeUnavailable;
    }

    /// @return Whether @p bytes is whole guest pages.
    [[nodiscard]] constexpr bool isWholePages(uint64_t bytes) noexcept
    {
        return bytes % proto::WasmPageBytes == 0;
    }
} // namespace

std::expected<SharedGuestMemory, ExtensionError> SharedGuestMemory::create(uint64_t capBytes,
                                                                           uint64_t initialBytes)
{
    if (initialBytes > capBytes || !isWholePages(initialBytes))
        return std::unexpected(ExtensionError::OutOfMemory);
    auto const objectBytes = proto::memoryObjectBytes(capBytes);
    auto object = transport::createSharedObject(objectBytes, SharedObjectUse::GuestMemory);
    if (!object)
        return std::unexpected(errorOf(object.error()));
    auto mapping = SharedMapping::map(object->get(), objectBytes, MapAccess::ReadWrite);
    if (!mapping)
        return std::unexpected(errorOf(mapping.error()));
    // Sealed once mapped: the object can no longer be resized, nor its seals changed, by anyone.
    if (auto const sealed = transport::sealSharedObject(object->get(), SharedObjectUse::GuestMemory); !sealed)
        return std::unexpected(errorOf(sealed.error()));
    if (auto const committed = mapping->commit(0, initialBytes); !committed)
        return std::unexpected(errorOf(committed.error()));
    return SharedGuestMemory { *std::move(object), *std::move(mapping), capBytes, initialBytes };
}

SharedGuestMemory::SharedGuestMemory(OwnedHandle object,
                                     SharedMapping mapping,
                                     uint64_t capBytes,
                                     uint64_t initialBytes) noexcept:
    _object { std::move(object) },
    _mapping { std::move(mapping) },
    _capBytes { capBytes },
    _acknowledged { initialBytes }
{
}

SharedGuestMemory::SharedGuestMemory(SharedGuestMemory&& other) noexcept:
    _object { std::move(other._object) },
    _mapping { std::move(other._mapping) },
    _capBytes { other._capBytes },
    _acknowledged { other._acknowledged.load(std::memory_order_relaxed) }
{
}

void SharedGuestMemory::closeObject() noexcept
{
    _object = OwnedHandle {};
}

proto::MemoryGrant SharedGuestMemory::grant(uint64_t wireValue) const noexcept
{
    return proto::MemoryGrant { .handle = wireValue,
                                .objectBytes = objectBytes(),
                                .initialBytes = acknowledgedBytes() };
}

std::expected<std::span<std::byte>, MemoryFailure> SharedGuestMemory::view(uint32_t address,
                                                                           uint32_t size) const noexcept
{
    auto const end = uint64_t { address } + size;
    if (end > acknowledgedBytes())
        return std::unexpected(MemoryFailure::OutOfBounds);
    return _mapping.bytes().subspan(address, size);
}

bool SharedGuestMemory::isAdmissible(uint64_t bytes) const noexcept
{
    return isWholePages(bytes) && bytes > acknowledgedBytes() && bytes <= _capBytes && bytes <= objectBytes();
}

GrowVerdict SharedGuestMemory::grow(uint64_t bytes) noexcept
{
    if (!isAdmissible(bytes))
        return GrowVerdict::Violation;
    // Committed before it is acknowledged: no view reaches a page that is not. And no further: on Windows a
    // commit is the section's, so a page committed here is open in the helper's view too, closed or not.
    if (!_mapping.commit(acknowledgedBytes(), bytes))
        return GrowVerdict::Refused;
    _acknowledged.store(bytes, std::memory_order_release);
    return GrowVerdict::Granted;
}

GrowVerdict SharedGuestMemory::refuseGrowth(uint64_t bytes) const noexcept
{
    return isAdmissible(bytes) ? GrowVerdict::Refused : GrowVerdict::Violation;
}

} // namespace vtextension::wasm::remote
