// SPDX-License-Identifier: Apache-2.0
//
// The interrupt page on Windows: a section made for the page (SharedObject.hpp), not inheritable until it is
// handed over. Contour maps it writable and then keeps only a handle that grants FILE_MAP_READ, the one it
// hands over: a well-behaved helper cannot map the page writable through it. A hostile one can: an unnamed
// section carries no security descriptor that is checked, so DuplicateHandle can widen the access of the
// helper's own handle. That grants nothing, because Contour only stores to the page and never reads it back;
// an AppContainer around the helper narrows it further.
#include <atomic>
#include <cstdint>
#include <expected>
#include <tuple>
#include <utility>

#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
              "the page is shared between processes: only a lock-free flag is address-free");

namespace
{
    /// @return A handle on @p handle's section that grants reading only; or `Io` if it cannot be made.
    [[nodiscard]] std::expected<OwnedHandle, SharedObjectError> readOnlyHandle(NativeHandle handle) noexcept
    {
        auto copy = HANDLE { nullptr };
        if (!::DuplicateHandle(
                ::GetCurrentProcess(), handle, ::GetCurrentProcess(), &copy, FILE_MAP_READ, FALSE, 0))
            return std::unexpected(SharedObjectError::Io);
        return OwnedHandle { copy };
    }
} // namespace

std::expected<ControlPage, SharedObjectError> ControlPage::create()
{
    // The view keeps the section alive; the full-access handle goes, and a read-only one stays.
    return createSharedObject(ControlPageBytes, SharedObjectUse::InterruptPage)
        .and_then([](OwnedHandle const& object) {
            return SharedMapping::map(object.get(), ControlPageBytes, MapAccess::ReadWrite)
                .and_then([&](SharedMapping mapping) {
                    return readOnlyHandle(object.get()).transform([&](OwnedHandle readOnly) {
                        return ControlPage { std::move(readOnly), std::move(mapping) };
                    });
                });
        });
}

std::expected<ControlPage, SharedObjectError> ControlPage::adopt(NativeHandle handle)
{
    return SharedMapping::map(handle, ControlPageBytes, MapAccess::ReadOnly)
        .and_then([handle](SharedMapping mapping) -> std::expected<ControlPage, SharedObjectError> {
            if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0))
                return std::unexpected(SharedObjectError::Io);
            return ControlPage { OwnedHandle { handle }, std::move(mapping) };
        });
}

std::expected<ControlPage, SharedObjectError> ControlPage::openHelperView() const
{
    return readOnlyHandle(_handle.get()).and_then([](OwnedHandle copy) {
        auto view = adopt(copy.get());
        if (view)
            std::ignore = copy.release();
        return view;
    });
}

} // namespace vtextension::wasm::remote::transport
