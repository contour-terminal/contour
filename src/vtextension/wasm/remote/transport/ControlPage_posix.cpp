// SPDX-License-Identifier: Apache-2.0
//
// The interrupt page on POSIX. Linux and FreeBSD: an anonymous memfd, sealed against shrinking and growing,
// so that neither process can make the other's mapping fault past the object's end. macOS has no memfd: a
// POSIX shared memory object under a name unique to this process, created exclusively and unlinked at once,
// so that it is reachable only through the descriptor.
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
            || ::fcntl(descriptor, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == -1)
        {
            auto const error = errno;
            ::close(descriptor);
            return std::unexpected(sharedObjectErrorOf(error));
        }
        return descriptor;
#endif
    }

    /// @return @p descriptor's page mapped shared, readable and writable; or why it could not be.
    [[nodiscard]] std::expected<uint32_t*, SharedObjectError> mapPage(int descriptor) noexcept
    {
        auto* const mapping =
            ::mmap(nullptr, ControlPageBytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        if (mapping == MAP_FAILED)
            return std::unexpected(SharedObjectError::Io);
        return static_cast<uint32_t*>(mapping);
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
    auto const flags = mapPage(*descriptor);
    if (!flags)
    {
        ::close(*descriptor);
        return std::unexpected(flags.error());
    }
    return ControlPage { *descriptor, *flags };
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
    return mapPage(handle).transform([&](uint32_t* flags) { return ControlPage { handle, flags }; });
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
