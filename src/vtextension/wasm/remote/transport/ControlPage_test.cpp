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
#include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/mman.h>
    #include <sys/wait.h>

    #include <cerrno>
    #include <csignal>
    #include <format>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using vtextension::wasm::remote::transport::ControlPage;
using vtextension::wasm::remote::transport::SharedObjectError;
using vtextension::wasm::remote::transport::testing::duplicate;

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

TEST_CASE("ControlPage.helperCannotWrite", "[vtextension]")
{
    auto parent = ControlPage::create();
    REQUIRE(parent.has_value());
#ifdef _WIN32
    // The handle handed over grants reading only.
    auto* const view = ::MapViewOfFile(parent->nativeHandle(), FILE_MAP_WRITE, 0, 0, 0);
    CHECK(view == nullptr);
    if (view != nullptr)
        ::UnmapViewOfFile(view);
#else
    #if defined(__linux__) && defined(F_SEAL_FUTURE_WRITE)
    // Sealed: no write mapping after Contour's own.
    auto* const mapping = ::mmap(nullptr,
                                 vtextension::wasm::remote::transport::ControlPageBytes,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED,
                                 parent->nativeHandle(),
                                 0);
    auto const error = errno;
    CHECK(mapping == MAP_FAILED);
    CHECK(error == EPERM);
    if (mapping != MAP_FAILED)
        ::munmap(mapping, vtextension::wasm::remote::transport::ControlPageBytes);
    #endif
    // An adopted page is mapped read-only: a store through it faults. Tried in a child, which the fault ends
    // (or the sanitizer's report of it, with a non-zero status).
    auto const copy = duplicate(parent->nativeHandle());
    REQUIRE(copy.has_value());
    auto const child = ::fork();
    REQUIRE(child != -1);
    if (child == 0)
    {
        // Quietly: neither the test framework's fault handler nor the sanitizer's may report from here.
        auto const nullDevice = ::open("/dev/null", O_WRONLY);
        if (nullDevice == -1)
            ::_exit(1);
        ::dup2(nullDevice, STDOUT_FILENO);
        ::dup2(nullDevice, STDERR_FILENO);
        struct sigaction byDefault {};
        byDefault.sa_handler = SIG_DFL;
        ::sigaction(SIGSEGV, &byDefault, nullptr);
        ::sigaction(SIGBUS, &byDefault, nullptr);
        auto helper = ControlPage::adopt(*copy);
        if (helper)
            helper->raise(5);
        ::_exit(0);
    }
    ::close(*copy);
    auto status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK_FALSE((WIFEXITED(status) && WEXITSTATUS(status) == 0));
    CHECK_FALSE(parent->isRaised(5));
#endif
}
