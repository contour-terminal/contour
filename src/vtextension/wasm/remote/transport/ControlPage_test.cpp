// SPDX-License-Identifier: Apache-2.0
//
// The page Contour and its extension host process both map: a flag raised through one mapping is seen
// through the other, the helper refuses a page of the wrong size, and nobody can resize it or inherit it.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

#include <vtextension/wasm/remote/transport/ControlPage.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/mman.h>

    #include <cerrno>
    #include <format>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using vtextension::wasm::remote::transport::ControlPage;
using vtextension::wasm::remote::transport::NativeHandle;
using vtextension::wasm::remote::transport::SharedObjectError;

namespace
{

/// @return A second handle on what @p handle names, as the helper inherits it; nothing if it failed.
[[nodiscard]] std::optional<NativeHandle> duplicate(NativeHandle handle) noexcept
{
#ifdef _WIN32
    auto copy = HANDLE { nullptr };
    if (!::DuplicateHandle(
            ::GetCurrentProcess(), handle, ::GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return std::nullopt;
    return copy;
#else
    auto const copy = ::dup(handle);
    if (copy == -1)
        return std::nullopt;
    return copy;
#endif
}

} // namespace

TEST_CASE("ControlPage.raiseIsSeenThroughASecondMapping", "[vtextension]")
{
    auto parent = ControlPage::create();
    REQUIRE(parent.has_value());
    auto const copy = duplicate(parent->nativeHandle());
    REQUIRE(copy.has_value());
    auto helper = ControlPage::adopt(*copy);
    REQUIRE(helper.has_value());

    parent->raise(5);
    CHECK(helper->isRaised(5));
    CHECK_FALSE(helper->isRaised(6));
    parent->clear(5);
    CHECK_FALSE(helper->isRaised(5));

    // The last slot is the page's last word.
    parent->raise(vtextension::wasm::remote::transport::InterruptSlotCount - 1);
    CHECK(helper->isRaised(vtextension::wasm::remote::transport::InterruptSlotCount - 1));

    // A moved page is the same mapping.
    auto moved = std::move(*helper);
    CHECK(moved.isRaised(vtextension::wasm::remote::transport::InterruptSlotCount - 1));
}

#ifndef _WIN32
TEST_CASE("ControlPage.adoptRefusesAWrongSize", "[vtextension]")
{
    #ifdef __APPLE__
    auto const name = std::format("/contour-eh-test-{}", ::getpid());
    auto const descriptor = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    REQUIRE(descriptor != -1);
    ::shm_unlink(name.c_str());
    #else
    auto const descriptor = ::memfd_create("contour-eh-test", MFD_CLOEXEC);
    REQUIRE(descriptor != -1);
    #endif
    REQUIRE(::ftruncate(descriptor, 8192) == 0);
    CHECK(ControlPage::adopt(descriptor) == std::unexpected(SharedObjectError::WrongSize));
    // A refused handle stays its owner's.
    CHECK(::close(descriptor) == 0);
}
#endif

#if defined(__linux__) || defined(__FreeBSD__)
TEST_CASE("ControlPage.isSealed", "[vtextension]")
{
    auto const page = ControlPage::create();
    REQUIRE(page.has_value());
    // Neither process can make the other's mapping run past the object's end, nor shrink it under it.
    for (auto const size: { 8192, 0 })
    {
        auto const resized = ::ftruncate(page->nativeHandle(), size);
        auto const error = errno;
        CHECK(resized == -1);
        CHECK(error == EPERM);
    }
}
#endif

TEST_CASE("ControlPage.isNotInherited", "[vtextension]")
{
    auto const page = ControlPage::create();
    REQUIRE(page.has_value());
#ifdef _WIN32
    auto flags = DWORD { 0 };
    REQUIRE(::GetHandleInformation(page->nativeHandle(), &flags));
    CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
#else
    auto const flags = ::fcntl(page->nativeHandle(), F_GETFD);
    REQUIRE(flags != -1);
    CHECK((flags & FD_CLOEXEC) != 0);
#endif
}
