// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Shared memory objects Contour and its extension host process both map, made in one place: the interrupt
/// page (ControlPage.hpp) and an instance's guest memory. Linux and FreeBSD make a memfd, sealed against
/// resizing; macOS a POSIX shared memory object under a name unique to this process, unlinked at once, sized
/// once; Windows an unnamed pagefile-backed section.
///
/// A mapping trusts no size the object reports beyond what fitsSharedObject() accepts: XNU rounds an object
/// up to its page, which may be larger than the page of the process that reads it (an x86_64 helper under
/// Rosetta sees 4 KiB pages, its arm64 parent rounded to 16 KiB).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>

namespace vtextension::wasm::remote::transport
{

/// The largest page a system this runs on rounds a shared memory object up to: 16 KiB, on Apple silicon.
inline constexpr uint64_t MaxPageBytes = uint64_t { 16 } * 1024;

/// Whether an object of @p objectBytes, as the system reports it, is one made of @p expectedBytes: at least
/// that, and at most that rounded up to MaxPageBytes, whatever the reader's own page. Whatever lies beyond
/// @p expectedBytes is never mapped.
/// @param objectBytes The size the system reports.
/// @param expectedBytes The size it was made of.
/// @return Whether it is that object's size.
[[nodiscard]] constexpr bool fitsSharedObject(uint64_t objectBytes, uint64_t expectedBytes) noexcept
{
    auto const remainder = expectedBytes % MaxPageBytes;
    auto const slack = remainder == 0 ? 0 : MaxPageBytes - remainder;
    return objectBytes >= expectedBytes && objectBytes - expectedBytes <= slack;
}

/// What a shared memory object is for, which decides how it is made and sealed.
enum class SharedObjectUse : uint8_t
{
    GuestMemory,   ///< An instance's memory: the helper writes it.
    InterruptPage, ///< The interrupt page: only Contour writes it. Sealed against later write mappings.
};

/// How a mapping may touch the object.
enum class MapAccess : uint8_t
{
    ReadOnly,  ///< It reads.
    ReadWrite, ///< It reads and writes.
};

/// Why a shared memory object could not be made, sealed or mapped.
enum class SharedObjectError : uint8_t
{
    Unsupported, ///< The system has no shared memory object of the kind needed.
    Io,          ///< The system refused.
    WrongSize,   ///< The object is not the size expected, or no object can have that size.
};

/// Makes a shared memory object of @p bytes, not inherited by a child unless handed over. Linux and FreeBSD
/// seal it against shrinking and growing at once; sealSharedObject() seals it for good once Contour mapped
/// it.
/// @param bytes Its size: more than 0.
/// @param use What it is for.
/// @return Its handle; `WrongSize` for a size no object has, `Unsupported` or `Io` if the system refused.
[[nodiscard]] std::expected<OwnedHandle, SharedObjectError> createSharedObject(uint64_t bytes,
                                                                               SharedObjectUse use);

/// Seals @p handle's object for good, once Contour mapped it: on Linux and FreeBSD against further seals,
/// and for the interrupt page, on Linux 5.1 and later, against any write mapping made after Contour's own.
/// Guest memory is never sealed against writes: the helper writes it. Nothing to do on macOS and Windows.
/// @param handle An object createSharedObject() made for @p use.
/// @param use What it is for.
/// @return Nothing once sealed; `Io` if the system refused.
[[nodiscard]] std::expected<void, SharedObjectError> sealSharedObject(NativeHandle handle,
                                                                      SharedObjectUse use) noexcept;

#ifndef _WIN32
/// Checks the size of the object @p handle names, as fstat() reports it.
/// @param handle A shared memory object.
/// @param expectedBytes The size it was made of.
/// @return Nothing if fitsSharedObject() accepts it; `WrongSize` if not, `Io` if it cannot be asked.
[[nodiscard]] std::expected<void, SharedObjectError> checkSharedObjectSize(NativeHandle handle,
                                                                           uint64_t expectedBytes) noexcept;
#endif

/// The first bytes of a shared memory object, mapped shared. Move-only; unmapped when destroyed. It does not
/// own the object's handle: the mapping keeps the object alive on its own.
class SharedMapping
{
  public:
    /// Maps the first @p bytes of @p handle's object, once its size is checked: by fstat() on POSIX
    /// (checkSharedObjectSize()), by the view's extent on Windows.
    /// @param handle The object; still the caller's.
    /// @param bytes How many bytes to map: the size the object was made of.
    /// @param access How the mapping may touch them.
    /// @return The mapping; `WrongSize` if the object is not of @p bytes, `Io` if it cannot be mapped.
    [[nodiscard]] static std::expected<SharedMapping, SharedObjectError> map(NativeHandle handle,
                                                                             uint64_t bytes,
                                                                             MapAccess access);

    SharedMapping(SharedMapping&& other) noexcept;
    SharedMapping& operator=(SharedMapping&& other) noexcept;
    SharedMapping(SharedMapping const&) = delete;
    SharedMapping& operator=(SharedMapping const&) = delete;
    ~SharedMapping();

    /// @return The mapped bytes; empty once moved from.
    [[nodiscard]] std::span<std::byte> bytes() const noexcept { return _bytes; }

  private:
    /// @param bytes The mapping, its own.
    /// @param access How it was mapped.
    SharedMapping(std::span<std::byte> bytes, MapAccess access) noexcept;

    /// Unmaps what it holds, if anything.
    void release() noexcept;

    std::span<std::byte> _bytes; ///< The mapping; empty once moved from.
    MapAccess _access;           ///< How it was mapped.
};

} // namespace vtextension::wasm::remote::transport
