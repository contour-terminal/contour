// SPDX-License-Identifier: Apache-2.0
//
// The interrupt page on POSIX. Linux and FreeBSD: an anonymous memfd, sealed against shrinking and growing,
// so that neither process can make the other's mapping fault past the object's end. macOS has no memfd: a
// POSIX shared memory object under a name unique to this process, created exclusively and unlinked at once,
// so that it is reachable only through the descriptor.
//
// The helper maps the page read-only. On Linux (5.1 and later) the memfd is also sealed against every write
// mapping made after Contour's own (F_SEAL_FUTURE_WRITE), so that a helper cannot map it writable even if it
// tried, nor mprotect() its mapping writable. Where the seal is missing -- an older kernel, FreeBSD, macOS --
// the read-only mapping is the helper's own choice; a helper that writes to the page anyway still misleads
// only itself, as Contour never reads it back.
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <vtextension/wasm/remote/transport/ControlPage.hpp>

#ifdef __APPLE__
    #include <format>
    #include <string>
#endif

namespace vtextension::wasm::remote::transport
{

static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
              "the page is shared between processes: only a lock-free flag is address-free");

namespace
{
    /// @return Why a call that set @p error failed.
    [[nodiscard]] SharedObjectError sharedObjectErrorOf(int error) noexcept
    {
        return error == ENOSYS || error == EINVAL ? SharedObjectError::Unsupported : SharedObjectError::Io;
    }

    /// @return A new shared memory object of ControlPageBytes, close-on-exec; or why it could not be made.
    [[nodiscard]] std::expected<int, SharedObjectError> createObject() noexcept
    {
#ifdef __APPLE__
        static auto serial = std::atomic<uint64_t> { 0 };
        auto const name = std::format("/contour-eh-{}-{}", ::getpid(), serial.fetch_add(1));
        auto const descriptor = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (descriptor == -1)
            return std::unexpected(sharedObjectErrorOf(errno));
        ::shm_unlink(name.c_str());
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1
            || ::ftruncate(descriptor, static_cast<off_t>(ControlPageBytes)) == -1)
        {
            auto const error = errno;
            ::close(descriptor);
            return std::unexpected(sharedObjectErrorOf(error));
        }
        return descriptor;
#else
        auto const descriptor =
            ::memfd_create("contour-extension-host-control", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (descriptor == -1)
            return std::unexpected(sharedObjectErrorOf(errno));
        if (::ftruncate(descriptor, static_cast<off_t>(ControlPageBytes)) == -1
            || ::fcntl(descriptor, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == -1)
        {
            auto const error = errno;
            ::close(descriptor);
            return std::unexpected(sharedObjectErrorOf(error));
        }
        return descriptor;
#endif
    }

    /// Who maps the page, and so what it may do to it.
    enum class PageAccess : uint8_t
    {
        ReadOnly,  ///< The helper: it reads the flags.
        ReadWrite, ///< Contour: it raises and clears them.
    };

    /// @return @p descriptor's page mapped shared with @p access; or why it could not be.
    [[nodiscard]] std::expected<uint32_t*, SharedObjectError> mapPage(int descriptor,
                                                                      PageAccess access) noexcept
    {
        auto const protection = access == PageAccess::ReadWrite ? PROT_READ | PROT_WRITE : PROT_READ;
        auto* const mapping = ::mmap(nullptr, ControlPageBytes, protection, MAP_SHARED, descriptor, 0);
        if (mapping == MAP_FAILED)
            return std::unexpected(SharedObjectError::Io);
        return static_cast<uint32_t*>(mapping);
    }

    /// Seals @p descriptor, once Contour mapped it: against any later write mapping where the system can, and
    /// against further seals. Nothing to do where the object is no memfd (macOS).
    /// @return Nothing; or why it could not be sealed.
    [[nodiscard]] std::expected<void, SharedObjectError> sealMapped([[maybe_unused]] int descriptor) noexcept
    {
#ifndef __APPLE__
    #ifdef F_SEAL_FUTURE_WRITE
        // EINVAL: a kernel older than the seal; the helper's read-only mapping is then the only guard.
        if (::fcntl(descriptor, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == -1 && errno != EINVAL)
            return std::unexpected(sharedObjectErrorOf(errno));
    #endif
        if (::fcntl(descriptor, F_ADD_SEALS, F_SEAL_SEAL) == -1)
            return std::unexpected(sharedObjectErrorOf(errno));
#endif
        return {};
    }
} // namespace

ControlPage::ControlPage(NativeHandle handle, uint32_t* flags) noexcept: _handle { handle }, _flags { flags }
{
}

std::expected<ControlPage, SharedObjectError> ControlPage::create()
{
    auto const descriptor = createObject();
    if (!descriptor)
        return std::unexpected(descriptor.error());
    auto const flags = mapPage(*descriptor, PageAccess::ReadWrite);
    if (!flags)
    {
        ::close(*descriptor);
        return std::unexpected(flags.error());
    }
    auto page = ControlPage { *descriptor, *flags };
    return sealMapped(*descriptor).transform([&] { return std::move(page); });
}

std::expected<ControlPage, SharedObjectError> ControlPage::adopt(NativeHandle handle)
{
    struct stat status {};
    if (::fstat(handle, &status) == -1)
        return std::unexpected(SharedObjectError::Io);
    if (std::cmp_not_equal(status.st_size, ControlPageBytes))
        return std::unexpected(SharedObjectError::WrongSize);
    if (::fcntl(handle, F_SETFD, FD_CLOEXEC) == -1)
        return std::unexpected(SharedObjectError::Io);
    return mapPage(handle, PageAccess::ReadOnly).transform([&](uint32_t* flags) {
        return ControlPage { handle, flags };
    });
}

ControlPage::ControlPage(ControlPage&& other) noexcept:
    _handle { std::exchange(other._handle, -1) }, _flags { std::exchange(other._flags, nullptr) }
{
}

ControlPage& ControlPage::operator=(ControlPage&& other) noexcept
{
    if (this != &other)
    {
        release();
        _handle = std::exchange(other._handle, -1);
        _flags = std::exchange(other._flags, nullptr);
    }
    return *this;
}

ControlPage::~ControlPage()
{
    release();
}

void ControlPage::release() noexcept
{
    if (_flags != nullptr)
        ::munmap(_flags, ControlPageBytes);
    if (_handle != -1)
        ::close(_handle);
    _flags = nullptr;
    _handle = -1;
}

NativeHandle ControlPage::nativeHandle() const noexcept
{
    return _handle;
}

} // namespace vtextension::wasm::remote::transport
