// SPDX-License-Identifier: Apache-2.0
//
// The interrupt page on POSIX: a shared memory object made for the page (SharedObject.hpp), mapped writable
// by Contour and sealed once it is.
//
// The helper maps the page read-only. On Linux (5.1 and later) the object is also sealed against every write
// mapping made after Contour's own, so that a helper cannot map it writable even if it tried, nor mprotect()
// its mapping writable. Where the seal is missing -- an older kernel, FreeBSD, macOS -- the read-only mapping
// is the helper's own choice; a helper that writes to the page anyway still misleads only itself, as Contour
// never reads it back.
#include <atomic>
#include <cstdint>
#include <expected>
#include <tuple>
#include <utility>

#include <fcntl.h>

#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>

namespace vtextension::wasm::remote::transport
{

static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
              "the page is shared between processes: only a lock-free flag is address-free");

std::expected<ControlPage, SharedObjectError> ControlPage::create()
{
    return createSharedObject(ControlPageBytes, SharedObjectUse::InterruptPage)
        .and_then([](OwnedHandle object) {
            return SharedMapping::map(object.get(), ControlPageBytes, MapAccess::ReadWrite)
                .and_then([&](SharedMapping mapping) {
                    return sealSharedObject(object.get(), SharedObjectUse::InterruptPage).transform([&] {
                        return ControlPage { std::move(object), std::move(mapping) };
                    });
                });
        });
}

std::expected<ControlPage, SharedObjectError> ControlPage::adopt(NativeHandle handle)
{
    // Not exactly ControlPageBytes: XNU reports a shared memory object rounded up to its page, which the
    // mapping checks for; only the first ControlPageBytes are mapped.
    return SharedMapping::map(handle, ControlPageBytes, MapAccess::ReadOnly)
        .and_then([handle](SharedMapping mapping) -> std::expected<ControlPage, SharedObjectError> {
            if (::fcntl(handle, F_SETFD, FD_CLOEXEC) == -1)
                return std::unexpected(SharedObjectError::Io);
            return ControlPage { OwnedHandle { handle }, std::move(mapping) };
        });
}

std::expected<ControlPage, SharedObjectError> ControlPage::openHelperView() const
{
    auto copy = OwnedHandle { ::fcntl(_handle.get(), F_DUPFD_CLOEXEC, 0) };
    if (!copy)
        return std::unexpected(SharedObjectError::Io);
    auto view = adopt(copy.get());
    if (view)
        std::ignore = copy.release();
    return view;
}

} // namespace vtextension::wasm::remote::transport
