// SPDX-License-Identifier: Apache-2.0
//
// The interrupt page on Windows: an unnamed, pagefile-backed section of one page, not inheritable until it is
// handed over. A section cannot be resized once created, so it needs no seal. Contour maps it writable and
// then keeps only a handle that grants FILE_MAP_READ, the one it hands over: a well-behaved helper cannot map
// the page writable through it. A hostile one can: an unnamed section carries no security descriptor that is
// checked, so DuplicateHandle can widen the access of the helper's own handle. That grants nothing, because
// Contour only stores to the page and never reads it back; an AppContainer around the helper narrows it
// further.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include <vtextension/wasm/remote/transport/ControlPage.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
              "the page is shared between processes: only a lock-free flag is address-free");

namespace
{
    /// @return @p section's whole view mapped with @p access (FILE_MAP_…), and its size; or why it could not
    /// be.
    [[nodiscard]] std::expected<std::pair<uint32_t*, std::size_t>, SharedObjectError> mapSection(
        HANDLE section, DWORD access) noexcept
    {
        auto* const view = ::MapViewOfFile(section, access, 0, 0, 0);
        if (view == nullptr)
            return std::unexpected(SharedObjectError::Io);
        auto region = MEMORY_BASIC_INFORMATION {};
        if (::VirtualQuery(view, &region, sizeof(region)) == 0)
        {
            ::UnmapViewOfFile(view);
            return std::unexpected(SharedObjectError::Io);
        }
        return std::pair { static_cast<uint32_t*>(view), static_cast<std::size_t>(region.RegionSize) };
    }
} // namespace

ControlPage::ControlPage(NativeHandle handle, uint32_t* flags) noexcept: _handle { handle }, _flags { flags }
{
}

std::expected<ControlPage, SharedObjectError> ControlPage::create()
{
    auto* const section =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, ControlPageBytes, nullptr);
    if (section == nullptr)
        return std::unexpected(SharedObjectError::Io);
    auto const mapped = mapSection(section, FILE_MAP_READ | FILE_MAP_WRITE);
    if (!mapped)
    {
        ::CloseHandle(section);
        return std::unexpected(mapped.error());
    }
    // The view keeps the section alive; the full-access handle goes, and a read-only one stays.
    auto readOnly = HANDLE { nullptr };
    auto const duplicated = ::DuplicateHandle(
        ::GetCurrentProcess(), section, ::GetCurrentProcess(), &readOnly, FILE_MAP_READ, FALSE, 0);
    ::CloseHandle(section);
    if (!duplicated)
    {
        ::UnmapViewOfFile(mapped->first);
        return std::unexpected(SharedObjectError::Io);
    }
    return ControlPage { readOnly, mapped->first };
}

std::expected<ControlPage, SharedObjectError> ControlPage::adopt(NativeHandle handle)
{
    auto const mapped = mapSection(handle, FILE_MAP_READ);
    if (!mapped)
        return std::unexpected(mapped.error());
    if (mapped->second != ControlPageBytes)
    {
        ::UnmapViewOfFile(mapped->first);
        return std::unexpected(SharedObjectError::WrongSize);
    }
    if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0))
    {
        ::UnmapViewOfFile(mapped->first);
        return std::unexpected(SharedObjectError::Io);
    }
    return ControlPage { handle, mapped->first };
}

ControlPage::ControlPage(ControlPage&& other) noexcept:
    _handle { std::exchange(other._handle, nullptr) }, _flags { std::exchange(other._flags, nullptr) }
{
}

ControlPage& ControlPage::operator=(ControlPage&& other) noexcept
{
    if (this != &other)
    {
        release();
        _handle = std::exchange(other._handle, nullptr);
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
        ::UnmapViewOfFile(_flags);
    if (_handle != nullptr)
        ::CloseHandle(_handle);
    _flags = nullptr;
    _handle = nullptr;
}

NativeHandle ControlPage::nativeHandle() const noexcept
{
    return _handle;
}

} // namespace vtextension::wasm::remote::transport
