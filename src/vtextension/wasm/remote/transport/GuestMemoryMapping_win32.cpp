// SPDX-License-Identifier: Apache-2.0
//
// Guest memory on Windows, through placeholders: a placeholder reservation of the object and its guard
// (VirtualAlloc2), split where the object ends, the section mapped over the object's placeholder
// (MapViewOfFile3, MEM_REPLACE_PLACEHOLDER) with write access, and the open pages committed through this view
// (VirtualAlloc, MEM_COMMIT): a reserved section's pages are committed by whoever touches them, so the helper
// commits what it opens itself, whether Windows commits for the section or for each view.
//
// The placeholder functions are Windows 10 1803's; they are looked up in kernelbase at run time, so that an
// older system says it cannot map guest memory (canMapGuestMemory()) rather than fail to load. A section too
// small for the view fails to map: that is the size check, as the placeholder's view cannot be measured the
// way SharedMapping measures a whole-section view.
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

namespace
{
    using VirtualAlloc2Fn =
        PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapViewOfFile3Fn =
        PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

    /// The placeholder functions, where the system has them.
    struct PlaceholderFunctions
    {
        VirtualAlloc2Fn virtualAlloc2 = nullptr;   ///< Reserves a placeholder.
        MapViewOfFile3Fn mapViewOfFile3 = nullptr; ///< Maps a view over a placeholder.
    };

    /// @return The placeholder functions; nothing on a system without them. Looked up once.
    [[nodiscard]] std::optional<PlaceholderFunctions> const& placeholderFunctions() noexcept
    {
        static auto const functions = []() -> std::optional<PlaceholderFunctions> {
            auto* const kernelbase = ::GetModuleHandleW(L"kernelbase.dll");
            if (kernelbase == nullptr)
                return std::nullopt;
            auto const found = PlaceholderFunctions {
                .virtualAlloc2 =
                    reinterpret_cast<VirtualAlloc2Fn>(::GetProcAddress(kernelbase, "VirtualAlloc2")),
                .mapViewOfFile3 =
                    reinterpret_cast<MapViewOfFile3Fn>(::GetProcAddress(kernelbase, "MapViewOfFile3")),
            };
            if (found.virtualAlloc2 == nullptr || found.mapViewOfFile3 == nullptr)
                return std::nullopt;
            return found;
        }();
        return functions;
    }

    /// Commits [@p from, @p from + @p bytes) through the view it lies in.
    /// @return Whether the system committed it.
    [[nodiscard]] std::expected<void, MappingError> commitThroughView(std::byte* from,
                                                                      uint64_t bytes) noexcept
    {
        if (bytes != 0
            && ::VirtualAlloc(from, static_cast<SIZE_T>(bytes), MEM_COMMIT, PAGE_READWRITE) == nullptr)
            return std::unexpected(MappingError::Io);
        return {};
    }
} // namespace

std::expected<GuestMemoryMapping, MappingError> GuestMemoryMapping::map(NativeHandle object,
                                                                        uint64_t objectBytes,
                                                                        uint64_t initialBytes,
                                                                        uint64_t guardBytes)
{
    if (!isGuestMemoryLayout(objectBytes, initialBytes, guardBytes))
        return std::unexpected(MappingError::WrongSize);
    auto const& functions = placeholderFunctions();
    if (!functions)
        return std::unexpected(MappingError::Unsupported);

    auto* const process = ::GetCurrentProcess();
    auto* const reservation =
        static_cast<std::byte*>(functions->virtualAlloc2(process,
                                                         nullptr,
                                                         static_cast<SIZE_T>(objectBytes + guardBytes),
                                                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                         PAGE_NOACCESS,
                                                         nullptr,
                                                         0));
    if (reservation == nullptr)
        return std::unexpected(MappingError::Io);
    // Two placeholders, the object's and the guard's, so that the view replaces exactly the first.
    if (guardBytes != 0
        && ::VirtualFree(
               reservation, static_cast<SIZE_T>(objectBytes), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)
               == 0)
    {
        ::VirtualFree(reservation, 0, MEM_RELEASE);
        return std::unexpected(MappingError::Io);
    }
    auto const freeReservation = [&] {
        ::VirtualFree(reservation, 0, MEM_RELEASE);
        if (guardBytes != 0)
            ::VirtualFree(reservation + objectBytes, 0, MEM_RELEASE);
    };
    if (functions->mapViewOfFile3(object,
                                  process,
                                  reservation,
                                  0,
                                  static_cast<SIZE_T>(objectBytes),
                                  MEM_REPLACE_PLACEHOLDER,
                                  PAGE_READWRITE,
                                  nullptr,
                                  0)
        == nullptr)
    {
        freeReservation();
        return std::unexpected(MappingError::Io);
    }
    // From here on the mapping owns the view and the guard, and releases them on every way out.
    auto mapping = GuestMemoryMapping { reservation, objectBytes, 0, guardBytes };
    return commitThroughView(reservation, initialBytes).transform([&] {
        mapping._accessibleBytes = initialBytes;
        return std::move(mapping);
    });
}

std::expected<void, MappingError> GuestMemoryMapping::openRange(uint64_t from, uint64_t to) noexcept
{
    return commitThroughView(_base + from, to - from);
}

void GuestMemoryMapping::release() noexcept
{
    if (_base == nullptr)
        return;
    // Unmapping a view that replaced a placeholder frees its range; the guard is a placeholder of its own.
    ::UnmapViewOfFile(_base);
    if (_guardBytes != 0)
        ::VirtualFree(_base + _objectBytes, 0, MEM_RELEASE);
    _base = nullptr;
}

bool canMapGuestMemory() noexcept
{
    static auto const canMap = [] {
        constexpr auto ProbeBytes = uint64_t { 64 } * 1024;
        auto const object = createSharedObject(ProbeBytes, SharedObjectUse::GuestMemory);
        if (!object)
            return false;
        auto const mapping = GuestMemoryMapping::map(object->get(), ProbeBytes, ProbeBytes, ProbeBytes);
        if (!mapping)
            return false;
        auto* const base = static_cast<std::byte volatile*>(mapping->base());
        base[ProbeBytes - 1] = std::byte { 0x5A };
        return base[ProbeBytes - 1] == std::byte { 0x5A };
    }();
    return canMap;
}

} // namespace vtextension::wasm::remote::transport
