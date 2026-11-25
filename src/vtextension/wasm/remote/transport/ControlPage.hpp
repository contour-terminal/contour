// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The interrupt page: one page of shared memory Contour and its extension host process both map, a flag per
/// interrupt slot. Contour raises an instance's slot to interrupt its running call; the helper, woken by the
/// wake signal (WakeSignal.hpp), scans the page for raised slots.
///
/// Contour only ever stores to the page, never reads it back, so a helper that scribbles over it can mislead
/// only itself.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>

#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::transport
{

/// The interrupt page's size: one page on every platform this runs on.
inline constexpr std::size_t ControlPageBytes = 4096;

/// How many interrupt slots the page holds: a 32-bit flag each.
inline constexpr uint32_t InterruptSlotCount = ControlPageBytes / sizeof(std::atomic<uint32_t>);

/// Whether a shared memory object of @p objectBytes, as fstat() reports it, is the interrupt page: at least
/// ControlPageBytes, and at most that rounded up to the system's page. XNU rounds a POSIX shared memory
/// object's size up to the page at ftruncate(), and fstat() reports the rounded size -- 16 KiB on Apple
/// silicon -- while a memfd and a Windows section keep the size they were given. Whatever lies beyond
/// ControlPageBytes is never mapped.
/// @param objectBytes The object's size.
/// @param pageBytes The system's page size, a power of two.
/// @return Whether it is the page's size.
[[nodiscard]] constexpr bool isControlPageSize(uint64_t objectBytes, uint64_t pageBytes) noexcept
{
    auto const rounded = (uint64_t { ControlPageBytes } + pageBytes - 1) / pageBytes * pageBytes;
    return objectBytes >= ControlPageBytes && objectBytes <= rounded;
}

/// Why a shared memory object could not be made or mapped.
enum class SharedObjectError : uint8_t
{
    Unsupported, ///< The system has no shared memory object of the kind needed.
    Io,          ///< The system refused.
    WrongSize,   ///< The object adopted is not the size expected.
};

/// One page both processes map: a flag per interrupt slot. Move-only; unmapped and closed when destroyed. A
/// moved-from page may only be assigned to or destroyed.
class ControlPage
{
  public:
    /// Creates the page, all slots clear: Contour's side. Not inherited by a child unless handed over.
    /// @return The page; `Unsupported` or `Io` if the system refused.
    [[nodiscard]] static std::expected<ControlPage, SharedObjectError> create();

    /// Maps a page this process inherited, read-only: the helper's side, which only reads the flags. Takes
    /// ownership of @p handle, made close-on-exec (not inheritable) again.
    /// @param handle The page's handle.
    /// @return The page; `WrongSize` if the object is not of the page's size (isControlPageSize()), `Io` if
    /// it cannot be
    ///         mapped. On failure @p handle stays the caller's.
    [[nodiscard]] static std::expected<ControlPage, SharedObjectError> adopt(NativeHandle handle);

    /// Maps this page a second time, read-only, on a handle of its own, as adopt() maps an inherited one:
    /// the helper's side of a host that serves in this process.
    /// @return The second mapping; `Io` if the handle could not be duplicated or the page mapped.
    [[nodiscard]] std::expected<ControlPage, SharedObjectError> openHelperView() const;

    ControlPage(ControlPage&& other) noexcept;
    ControlPage& operator=(ControlPage&& other) noexcept;
    ControlPage(ControlPage const&) = delete;
    ControlPage& operator=(ControlPage const&) = delete;
    ~ControlPage();

    /// Raises @p slot. Wait-free. Only on the page create() made: an adopted page is mapped read-only.
    /// @param slot The slot; below InterruptSlotCount, or else nothing happens.
    void raise(uint32_t slot) noexcept;

    /// Clears @p slot. Wait-free. Only on the page create() made: an adopted page is mapped read-only.
    /// @param slot The slot; below InterruptSlotCount, or else nothing happens.
    void clear(uint32_t slot) noexcept;

    /// @param slot The slot.
    /// @return Whether @p slot is raised; never for one at or beyond InterruptSlotCount.
    [[nodiscard]] bool isRaised(uint32_t slot) const noexcept;

    /// @return The page's handle, still owned by the page: for handing it to a child process. On Windows it
    ///         grants read access only, so that a well-behaved child cannot map the page writable; a hostile
    ///         one can widen its own handle (an unnamed section's access is not checked), which grants it
    ///         nothing, since this process never reads the page back.
    [[nodiscard]] NativeHandle nativeHandle() const noexcept;

  private:
    ControlPage(NativeHandle handle, uint32_t* flags) noexcept;

    /// @return @p slot's flag.
    [[nodiscard]] std::atomic_ref<uint32_t> flag(uint32_t slot) const noexcept;

    /// Unmaps and closes what this page holds, if anything.
    void release() noexcept;

    NativeHandle _handle;
    uint32_t* _flags; ///< The mapping, InterruptSlotCount flags; null once moved from.
};

inline std::atomic_ref<uint32_t> ControlPage::flag(uint32_t slot) const noexcept
{
    return std::atomic_ref<uint32_t> { _flags[slot] };
}

inline void ControlPage::raise(uint32_t slot) noexcept
{
    // Release: what the raiser wrote before it raised is seen by whoever sees the flag raised.
    if (slot < InterruptSlotCount)
        flag(slot).store(1, std::memory_order_release);
}

inline void ControlPage::clear(uint32_t slot) noexcept
{
    if (slot < InterruptSlotCount)
        flag(slot).store(0, std::memory_order_release);
}

inline bool ControlPage::isRaised(uint32_t slot) const noexcept
{
    return slot < InterruptSlotCount && flag(slot).load(std::memory_order_acquire) != 0;
}

} // namespace vtextension::wasm::remote::transport
