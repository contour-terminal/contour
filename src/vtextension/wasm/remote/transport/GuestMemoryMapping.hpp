// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's mapping of an instance's memory object: the object Contour made and granted it,
/// mapped writable at the base of a reservation that also holds the guard region the engine's compiled code
/// relies on, with only the bytes the guest's memory spans open. Growing opens more of the object, never
/// moves it, and never closes what was open.
///
/// Every size is a whole number of guest pages (64 KiB), which is also Windows' allocation granularity, so
/// that a placeholder splits there.

#include <cstddef>
#include <cstdint>
#include <expected>

#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::transport
{

/// Why an object could not be mapped, or more of it opened.
enum class MappingError : uint8_t
{
    WrongSize,   ///< The object is not the size it was granted as, or a size asked for is not one it can be.
    Io,          ///< The system refused: the handle names no object, or one that cannot be mapped writable.
    Unsupported, ///< This system cannot place a shared object inside a reservation.
};

/// An instance's memory object, mapped into the extension host: [0, objectBytes()) is the object, followed by
/// guardBytes() that are never accessible; of the object only [0, accessibleBytes()) is open. Move-only;
/// unmapped, guard included, when destroyed. It does not own the object's handle: the mapping keeps the
/// object alive on its own.
class GuestMemoryMapping
{
  public:
    /// Maps @p object at the base of a fresh reservation of @p objectBytes + @p guardBytes, and opens its
    /// first @p initialBytes for reading and writing. The object must be mappable writable, and of
    /// @p objectBytes: on POSIX its size is checked (fitsSharedObject()); on Windows a section too small for
    /// the view fails to map.
    /// @param object The object's handle; still the caller's.
    /// @param objectBytes The size it was granted as: whole guest pages, more than 0.
    /// @param initialBytes How many of its bytes are open at first: whole guest pages, at most @p
    /// objectBytes.
    /// @param guardBytes How many inaccessible bytes follow it: whole guest pages.
    /// @return The mapping; `WrongSize` for sizes that do not fit those rules or an object of another size,
    ///         `Io` if the system refused, `Unsupported` if this system has no way to place it.
    [[nodiscard]] static std::expected<GuestMemoryMapping, MappingError> map(NativeHandle object,
                                                                             uint64_t objectBytes,
                                                                             uint64_t initialBytes,
                                                                             uint64_t guardBytes);

    GuestMemoryMapping(GuestMemoryMapping&& other) noexcept;
    GuestMemoryMapping& operator=(GuestMemoryMapping&& other) noexcept;
    GuestMemoryMapping(GuestMemoryMapping const&) = delete;
    GuestMemoryMapping& operator=(GuestMemoryMapping const&) = delete;
    ~GuestMemoryMapping();

    /// @return Where the object is mapped: the guest's byte 0. Null once moved from.
    [[nodiscard]] std::byte* base() const noexcept { return _base; }

    /// @return How many bytes from base() are open: the guest memory's current size.
    [[nodiscard]] uint64_t accessibleBytes() const noexcept { return _accessibleBytes; }

    /// @return The object's size: the most the guest memory may span.
    [[nodiscard]] uint64_t objectBytes() const noexcept { return _objectBytes; }

    /// @return How many inaccessible bytes follow the object.
    [[nodiscard]] uint64_t guardBytes() const noexcept { return _guardBytes; }

    /// Opens the object's first @p bytes for reading and writing; on Windows by committing the pages through
    /// this view. Never closes what is open: asking for fewer bytes than are open does nothing.
    /// @param bytes How many bytes are to be open: whole guest pages, at most objectBytes().
    /// @return Nothing once they are open; `WrongSize` for a size that does not fit, `Io` if the system
    ///         refused.
    [[nodiscard]] std::expected<void, MappingError> open(uint64_t bytes) noexcept;

  private:
    GuestMemoryMapping(std::byte* base,
                       uint64_t objectBytes,
                       uint64_t accessibleBytes,
                       uint64_t guardBytes) noexcept;

    /// Opens [@p from, @p to), whole guest pages within the object, as this system does.
    [[nodiscard]] std::expected<void, MappingError> openRange(uint64_t from, uint64_t to) noexcept;

    /// Unmaps the object and frees the reservation, if it holds them.
    void release() noexcept;

    std::byte* _base = nullptr;    ///< The object's first byte; null once moved from.
    uint64_t _objectBytes = 0;     ///< The object's size.
    uint64_t _accessibleBytes = 0; ///< How much of it is open.
    uint64_t _guardBytes = 0;      ///< The inaccessible bytes after it.
};

/// Whether sizes are ones GuestMemoryMapping::map() takes: whole guest pages, an object of at least one, its
/// initial bytes within it, and the whole reservation addressable.
/// @param objectBytes The object's size.
/// @param initialBytes How many of its bytes open at first.
/// @param guardBytes How many inaccessible bytes follow it.
/// @return Whether they are.
[[nodiscard]] bool isGuestMemoryLayout(uint64_t objectBytes,
                                       uint64_t initialBytes,
                                       uint64_t guardBytes) noexcept;

/// Whether this system can map guest memory as GuestMemoryMapping does: always on POSIX; on Windows only
/// where the placeholder functions exist (Windows 10 1803 and later) and a 64 KiB probe maps, opens and
/// writes. Asked once; the answer is kept.
/// @return Whether it can.
[[nodiscard]] bool canMapGuestMemory() noexcept;

} // namespace vtextension::wasm::remote::transport
