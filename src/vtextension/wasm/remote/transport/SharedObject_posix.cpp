// SPDX-License-Identifier: Apache-2.0
//
// Shared memory objects on POSIX. Linux and FreeBSD (13 and later): an anonymous memfd, sealed against
// shrinking and growing as it is made, so that neither process can make the other's mapping fault past the
// object's end, and against further seals once Contour mapped it. macOS has no memfd: a POSIX shared memory
// object under a name unique to this process, created exclusively and unlinked at once, so that it is
// reachable only through the descriptor. XNU sizes such an object once, and rounds it up to its page.
//
// The interrupt page is also sealed, on Linux 5.1 and later, against every write mapping made after
// Contour's own (F_SEAL_FUTURE_WRITE): the helper only reads it. Guest memory never is: the helper writes it.
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <vtextension/wasm/remote/transport/SharedObject.hpp>

#ifdef __APPLE__
    #include <atomic>
    #include <format>
    #include <string>
#endif

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// Whether an object is sealed against write mappings made after Contour's own.
    enum class LaterWrites : uint8_t
    {
        Allowed, ///< The helper maps it writable.
        Sealed,  ///< Nobody maps it writable after Contour (F_SEAL_FUTURE_WRITE, where there is one).
    };

    /// How an object for one use is made and sealed.
    struct UseRow
    {
        SharedObjectUse use;     ///< The use.
        char const* memfdName;   ///< The memfd's name, as /proc shows it (Linux, FreeBSD).
        LaterWrites laterWrites; ///< Whether it is sealed against later write mappings.
    };

    /// Every use, at its enumerator's index.
    constexpr auto Uses = std::array {
        UseRow { .use = SharedObjectUse::GuestMemory,
                 .memfdName = "contour-extension-host-memory",
                 .laterWrites = LaterWrites::Allowed },
        UseRow { .use = SharedObjectUse::InterruptPage,
                 .memfdName = "contour-extension-host-control",
                 .laterWrites = LaterWrites::Sealed },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, Uses.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(Uses[index].use) == index;
                                      }));

    /// @return How an object for @p use is made and sealed.
    [[nodiscard]] UseRow const& rowOf(SharedObjectUse use) noexcept
    {
        return Uses[std::to_underlying(use)];
    }

    /// @return Why a call that set @p error failed.
    [[nodiscard]] SharedObjectError sharedObjectErrorOf(int error) noexcept
    {
        return error == ENOSYS || error == EINVAL ? SharedObjectError::Unsupported : SharedObjectError::Io;
    }

    /// @return Nothing if @p result is not -1; why the call failed otherwise.
    [[nodiscard]] std::expected<void, SharedObjectError> succeeded(int result) noexcept
    {
        if (result == -1)
            return std::unexpected(sharedObjectErrorOf(errno));
        return {};
    }

    /// @return A new object, close-on-exec and not yet sized; or why it could not be made.
    [[nodiscard]] std::expected<OwnedHandle, SharedObjectError> openObject(
        [[maybe_unused]] UseRow const& row) noexcept
    {
#ifdef __APPLE__
        static auto serial = std::atomic<uint64_t> { 0 };
        auto const name = std::format("/contour-eh-{}-{}", ::getpid(), serial.fetch_add(1));
        auto object = OwnedHandle { ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600) };
        if (!object)
            return std::unexpected(sharedObjectErrorOf(errno));
        ::shm_unlink(name.c_str());
        return succeeded(::fcntl(object.get(), F_SETFD, FD_CLOEXEC)).transform([&] {
            return std::move(object);
        });
#else
        auto object = OwnedHandle { ::memfd_create(row.memfdName, MFD_CLOEXEC | MFD_ALLOW_SEALING) };
        if (!object)
            return std::unexpected(sharedObjectErrorOf(errno));
        return object;
#endif
    }
} // namespace

std::expected<OwnedHandle, SharedObjectError> createSharedObject(uint64_t bytes, SharedObjectUse use)
{
    if (bytes == 0 || bytes > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return std::unexpected(SharedObjectError::WrongSize);
    // Sized once -- all XNU allows -- and, where there are seals, sealed at that size at once.
    return openObject(rowOf(use)).and_then([bytes](OwnedHandle object) {
        return succeeded(::ftruncate(object.get(), static_cast<off_t>(bytes)))
#ifndef __APPLE__
            .and_then(
                [&] { return succeeded(::fcntl(object.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)); })
#endif
            .transform([&] { return std::move(object); });
    });
}

std::expected<void, SharedObjectError> sealSharedObject([[maybe_unused]] NativeHandle handle,
                                                        [[maybe_unused]] SharedObjectUse use) noexcept
{
#ifndef __APPLE__
    #ifdef F_SEAL_FUTURE_WRITE
    // EINVAL: a kernel older than the seal; the helper's read-only mapping is then the only guard.
    if (rowOf(use).laterWrites == LaterWrites::Sealed
        && ::fcntl(handle, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == -1 && errno != EINVAL)
        return std::unexpected(sharedObjectErrorOf(errno));
    #endif
    return succeeded(::fcntl(handle, F_ADD_SEALS, F_SEAL_SEAL));
#else
    return {};
#endif
}

std::expected<void, SharedObjectError> checkSharedObjectSize(NativeHandle handle,
                                                             uint64_t expectedBytes) noexcept
{
    struct stat status {};
    if (::fstat(handle, &status) == -1)
        return std::unexpected(SharedObjectError::Io);
    if (status.st_size < 0 || !fitsSharedObject(static_cast<uint64_t>(status.st_size), expectedBytes))
        return std::unexpected(SharedObjectError::WrongSize);
    return {};
}

std::expected<SharedMapping, SharedObjectError> SharedMapping::map(NativeHandle handle,
                                                                   uint64_t bytes,
                                                                   MapAccess access)
{
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max())
        return std::unexpected(SharedObjectError::WrongSize);
    return checkSharedObjectSize(handle, bytes)
        .and_then([&]() -> std::expected<SharedMapping, SharedObjectError> {
            auto const protection = access == MapAccess::ReadWrite ? PROT_READ | PROT_WRITE : PROT_READ;
            auto* const mapping =
                ::mmap(nullptr, static_cast<std::size_t>(bytes), protection, MAP_SHARED, handle, 0);
            if (mapping == MAP_FAILED)
                return std::unexpected(SharedObjectError::Io);
            return SharedMapping {
                std::span { static_cast<std::byte*>(mapping), static_cast<std::size_t>(bytes) }, access
            };
        });
}

std::expected<void, SharedObjectError> SharedMapping::commitRange([[maybe_unused]] uint64_t from,
                                                                  [[maybe_unused]] uint64_t to) noexcept
{
    // Every page of the object is backed on first touch.
    return {};
}

void SharedMapping::release() noexcept
{
    if (!_bytes.empty())
        ::munmap(_bytes.data(), _bytes.size());
    _bytes = {};
}

} // namespace vtextension::wasm::remote::transport
