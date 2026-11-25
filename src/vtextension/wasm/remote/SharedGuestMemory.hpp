// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An instance's linear memory as the parent holds it while its host shares memory rather than copying it:
/// the object both processes map, made by the parent, and the size the parent acknowledged the memory spans.
///
/// The helper reports nothing the parent believes about the memory's size. It asks to grow (Grow), and the
/// parent decides: what it granted is the one bound every view is checked against, whatever the helper's own
/// mapping spans. A view of the parent's mapping stays where it is, whatever the memory grows to.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>

namespace vtextension::wasm::remote
{

/// How the parent answers a host that asks to grow an instance's memory.
enum class GrowVerdict : uint8_t
{
    Violation, ///< It asked for what no honest host does: the host is killed, and nothing is answered.
    Refused,   ///< It is answered Refused: the guest's growth fails.
    Granted,   ///< It is answered Granted: the memory spans the new size from now on.
};

/// @param verdict A verdict other than `Violation`, which is answered with nothing.
/// @return What the host is answered with.
[[nodiscard]] constexpr proto::GrowStatus growStatusOf(GrowVerdict verdict) noexcept
{
    return verdict == GrowVerdict::Granted ? proto::GrowStatus::Granted : proto::GrowStatus::Refused;
}

/// One instance's memory object, made and mapped whole by the parent, and the size the parent acknowledged.
///
/// Thread-safe as the parent uses it: growth is answered by the thread calling into the instance, and views
/// may be taken by any thread; the acknowledged size is published with release and read with acquire. Its
/// handle is the parent's only until the Instantiate carrying it is sent, and is closed then (closeObject()):
/// the mapping keeps the object alive, so that no live instance holds a descriptor.
class SharedGuestMemory
{
  public:
    /// Makes the object of an instance whose runtime caps memory at @p capBytes, maps it whole, seals it, and
    /// opens the bytes the memory spans at first.
    /// @param capBytes The runtime's memory cap; the object is proto::memoryObjectBytes() of it.
    /// @param initialBytes The memory's initial size: whole guest pages, at most @p capBytes.
    /// @return The memory; `OutOfMemory` if @p initialBytes is past the cap or not whole guest pages, or the
    ///         system has no memory for the object; `RuntimeUnavailable` if the system refused otherwise.
    [[nodiscard]] static std::expected<SharedGuestMemory, ExtensionError> create(uint64_t capBytes,
                                                                                 uint64_t initialBytes);

    /// Moves the memory. Not thread-safe: only before any other thread can reach it.
    SharedGuestMemory(SharedGuestMemory&& other) noexcept;
    SharedGuestMemory& operator=(SharedGuestMemory&&) = delete;
    SharedGuestMemory(SharedGuestMemory const&) = delete;
    SharedGuestMemory& operator=(SharedGuestMemory const&) = delete;
    ~SharedGuestMemory() = default;

    /// @return The object's handle, for granting it; NoNativeHandle once closeObject() ran.
    [[nodiscard]] transport::NativeHandle object() const noexcept { return _object.get(); }

    /// Closes the object's handle, once the Instantiate that grants it was sent (or never will be). The
    /// mapping, and so the object, lives on.
    void closeObject() noexcept;

    /// @param wireValue What the grant names the object by (transport::HandleGrant::wireValue).
    /// @return What the Instantiate that hands the object over carries.
    [[nodiscard]] proto::MemoryGrant grant(uint64_t wireValue) const noexcept;

    /// A view of [@p address, @p address + @p size) of the memory, bounded by the acknowledged size, never by
    /// what the host reports. Valid as long as the memory: growth moves nothing.
    /// @param address The guest address the view starts at.
    /// @param size The view's size in bytes.
    /// @return The view; `OutOfBounds` if any of it lies past the acknowledged size.
    [[nodiscard]] std::expected<std::span<std::byte>, MemoryFailure> view(uint32_t address,
                                                                          uint32_t size) const noexcept;

    /// Answers a host that asks to grow the memory to @p bytes: a violation unless that is whole guest pages,
    /// more than acknowledged, and within both the cap and the object. Otherwise the new pages are committed
    /// (Windows), the size is acknowledged, and it is granted; it is refused if they cannot be committed.
    /// @param bytes The size the host asks for.
    /// @return The verdict.
    [[nodiscard]] GrowVerdict grow(uint64_t bytes) noexcept;

    /// Answers a host that asks to grow the memory to @p bytes when no growth may be granted -- its instance
    /// was interrupted: a violation as grow() judges one, refused otherwise. Acknowledges nothing.
    /// @param bytes The size the host asks for.
    /// @return The verdict: `Violation` or `Refused`.
    [[nodiscard]] GrowVerdict refuseGrowth(uint64_t bytes) const noexcept;

    /// @return How many bytes the parent acknowledged the memory spans.
    [[nodiscard]] uint64_t acknowledgedBytes() const noexcept
    {
        return _acknowledged.load(std::memory_order_acquire);
    }

    /// @return The object's size.
    [[nodiscard]] uint64_t objectBytes() const noexcept { return _mapping.bytes().size(); }

  private:
    SharedGuestMemory(transport::OwnedHandle object,
                      transport::SharedMapping mapping,
                      uint64_t capBytes,
                      uint64_t initialBytes) noexcept;

    /// @return Whether a host may ask to grow the memory to @p bytes at all.
    [[nodiscard]] bool isAdmissible(uint64_t bytes) const noexcept;

    transport::OwnedHandle _object;      ///< The object's handle, until the grant was sent.
    transport::SharedMapping _mapping;   ///< The whole object, mapped read-write; it never moves.
    uint64_t _capBytes;                  ///< The runtime's memory cap.
    std::atomic<uint64_t> _acknowledged; ///< The size the memory spans, as the parent acknowledged it.
};

} // namespace vtextension::wasm::remote
