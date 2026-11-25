// SPDX-License-Identifier: Apache-2.0
//
// The shared memory objects Contour and its extension host process both map: a size rounded up to any page a
// parent may use is accepted and no more, what one mapping writes the other sees, and nobody can resize an
// object, inherit it unasked, or find more of a reserved one committed than was asked for.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <utility>

#include <vtextension/wasm/remote/transport/SharedObject.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/mman.h>
    #include <sys/types.h>

    #include <cerrno>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using vtextension::wasm::remote::transport::createSharedObject;
using vtextension::wasm::remote::transport::fitsSharedObject;
using vtextension::wasm::remote::transport::MapAccess;
using vtextension::wasm::remote::transport::sealSharedObject;
using vtextension::wasm::remote::transport::SharedMapping;
using vtextension::wasm::remote::transport::SharedObjectError;
using vtextension::wasm::remote::transport::SharedObjectUse;
#ifndef _WIN32
using vtextension::wasm::remote::transport::checkSharedObjectSize;
#endif

namespace
{

constexpr auto KiB = uint64_t { 1024 };
constexpr auto MiB = 1024 * KiB;

/// The size the tests' guest memory is made of: a guest's page.
constexpr auto GuestBytes = 64 * KiB;

} // namespace

TEST_CASE("SharedObject.sizeMayBeRoundedUpToTheLargestPage", "[vtextension]")
{
    // An arm64 parent's 4 KiB object, rounded to its 16 KiB page, read by an x86_64 helper under Rosetta,
    // whose own page is 4 KiB.
    STATIC_CHECK(fitsSharedObject(16 * KiB, 4 * KiB));
    STATIC_CHECK(fitsSharedObject(4 * KiB, 4 * KiB));
    STATIC_CHECK(fitsSharedObject(32 * MiB, 32 * MiB));
    STATIC_CHECK_FALSE(fitsSharedObject((16 * KiB) + 1, 4 * KiB));
    STATIC_CHECK_FALSE(fitsSharedObject((4 * KiB) - 1, 4 * KiB));
    STATIC_CHECK_FALSE(fitsSharedObject((32 * MiB) + (16 * KiB), 32 * MiB));
    // No size near the top wraps around.
    STATIC_CHECK_FALSE(fitsSharedObject(0, UINT64_MAX));
    STATIC_CHECK(fitsSharedObject(UINT64_MAX, UINT64_MAX));
}

TEST_CASE("SharedObject.writesAreSeenThroughASecondMapping", "[vtextension]")
{
    auto const object = createSharedObject(GuestBytes, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto first = SharedMapping::map(object->get(), GuestBytes, MapAccess::ReadWrite);
    REQUIRE(first.has_value());
    auto second = SharedMapping::map(object->get(), GuestBytes, MapAccess::ReadWrite);
    REQUIRE(second.has_value());
    REQUIRE(sealSharedObject(object->get(), SharedObjectUse::GuestMemory).has_value());
    CHECK(first->bytes().size() == GuestBytes);
    REQUIRE(first->commit(0, GuestBytes).has_value());
    REQUIRE(second->commit(0, GuestBytes).has_value());

    first->bytes().front() = std::byte { 0x5A };
    second->bytes().back() = std::byte { 0xA5 };
    CHECK(second->bytes().front() == std::byte { 0x5A });
    CHECK(first->bytes().back() == std::byte { 0xA5 });

    // A moved mapping is the same mapping.
    auto moved = std::move(*second);
    CHECK(moved.bytes().front() == std::byte { 0x5A });

    // Nothing past the mapping commits.
    CHECK(first->commit(0, GuestBytes + 1) == std::unexpected(SharedObjectError::OutOfRange));
    CHECK(first->commit(2, 1) == std::unexpected(SharedObjectError::OutOfRange));
}

TEST_CASE("SharedObject.noObjectIsEmpty", "[vtextension]")
{
    for (auto const use: { SharedObjectUse::GuestMemory, SharedObjectUse::InterruptPage })
        CHECK(createSharedObject(0, use) == std::unexpected(SharedObjectError::WrongSize));
}

#if defined(__linux__) || defined(__FreeBSD__)
TEST_CASE("SharedObject.isSealedAgainstResizing", "[vtextension]")
{
    auto const object = createSharedObject(GuestBytes, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    // Against resizing from the start: nobody can make another's mapping fault past the end, nor shrink it.
    CHECK(::fcntl(object->get(), F_GET_SEALS) == (F_SEAL_SHRINK | F_SEAL_GROW));
    auto const mapping = SharedMapping::map(object->get(), GuestBytes, MapAccess::ReadWrite);
    REQUIRE(mapping.has_value());
    REQUIRE(sealSharedObject(object->get(), SharedObjectUse::GuestMemory).has_value());
    for (auto const size: { 2 * GuestBytes, uint64_t { 0 } })
    {
        auto const resized = ::ftruncate(object->get(), static_cast<off_t>(size));
        auto const error = errno;
        CHECK(resized == -1);
        CHECK(error == EPERM);
    }
    // Guest memory stays writable for the helper: sealed against resizing and further seals, never against
    // writes.
    CHECK(::fcntl(object->get(), F_GET_SEALS) == (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL));
    auto* const helperView =
        ::mmap(nullptr, GuestBytes, PROT_READ | PROT_WRITE, MAP_SHARED, object->get(), 0);
    CHECK(helperView != MAP_FAILED);
    if (helperView != MAP_FAILED)
        ::munmap(helperView, GuestBytes);
}
#endif

TEST_CASE("SharedObject.isNotInherited", "[vtextension]")
{
    for (auto const use: { SharedObjectUse::GuestMemory, SharedObjectUse::InterruptPage })
    {
        auto const object = createSharedObject(GuestBytes, use);
        REQUIRE(object.has_value());
#ifdef _WIN32
        auto flags = DWORD { 0 };
        REQUIRE(::GetHandleInformation(object->get(), &flags));
        CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
#else
        auto const flags = ::fcntl(object->get(), F_GETFD);
        REQUIRE(flags != -1);
        CHECK((flags & FD_CLOEXEC) != 0);
#endif
    }
}

TEST_CASE("SharedObject.checkRefusesAWrongSize", "[vtextension]")
{
    // Made of 32 KiB and expected as 4: more than any page rounds 4 KiB up to. Made of 4 KiB and expected as
    // 32: less.
    auto const large = createSharedObject(32 * KiB, SharedObjectUse::GuestMemory);
    REQUIRE(large.has_value());
    auto const small = createSharedObject(4 * KiB, SharedObjectUse::GuestMemory);
    REQUIRE(small.has_value());
    CHECK(SharedMapping::map(large->get(), 4 * KiB, MapAccess::ReadOnly)
          == std::unexpected(SharedObjectError::WrongSize));
    CHECK(SharedMapping::map(small->get(), 32 * KiB, MapAccess::ReadOnly)
          == std::unexpected(SharedObjectError::WrongSize));
    CHECK(SharedMapping::map(small->get(), 4 * KiB, MapAccess::ReadOnly).has_value());
#ifndef _WIN32
    CHECK(checkSharedObjectSize(large->get(), 4 * KiB) == std::unexpected(SharedObjectError::WrongSize));
    CHECK(checkSharedObjectSize(small->get(), 32 * KiB) == std::unexpected(SharedObjectError::WrongSize));
    CHECK(checkSharedObjectSize(small->get(), 4 * KiB).has_value());
    // What names no object cannot be asked.
    CHECK(checkSharedObjectSize(-1, 4 * KiB) == std::unexpected(SharedObjectError::Io));
#endif
}

#ifdef _WIN32
TEST_CASE("SharedObject.reservedSectionCommitsOnlyWhatIsAsked", "[vtextension]")
{
    // Guest memory is reserved: only the pages committed through a view are backed, and charged.
    auto const object = createSharedObject(MiB, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto mapping = SharedMapping::map(object->get(), MiB, MapAccess::ReadWrite);
    REQUIRE(mapping.has_value());
    auto* const base = mapping->bytes().data();

    auto region = MEMORY_BASIC_INFORMATION {};
    REQUIRE(::VirtualQuery(base, &region, sizeof(region)) != 0);
    CHECK(region.State == MEM_RESERVE);

    REQUIRE(mapping->commit(0, GuestBytes).has_value());
    REQUIRE(::VirtualQuery(base, &region, sizeof(region)) != 0);
    CHECK(region.State == MEM_COMMIT);
    CHECK(region.RegionSize == GuestBytes);
    REQUIRE(::VirtualQuery(base + GuestBytes, &region, sizeof(region)) != 0);
    CHECK(region.State == MEM_RESERVE);

    mapping->bytes()[GuestBytes - 1] = std::byte { 1 };
    CHECK(mapping->bytes()[GuestBytes - 1] == std::byte { 1 });
}

namespace
{

/// Whether this runs under wine, which commits a reserved section per view rather than per section.
[[nodiscard]] bool runsUnderWine() noexcept
{
    auto* const ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && ::GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

} // namespace

TEST_CASE("SharedObject.pagesCommittedThroughOneViewAreSeenThroughAnother", "[vtextension]")
{
    // Contour commits guest memory through its own view only; the helper's view must then reach the same
    // pages. Windows commits a reserved section's pages for the section, so any view may touch them.
    if (runsUnderWine())
        SKIP("wine commits a reserved section per view, unlike Windows");

    auto const object = createSharedObject(MiB, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto parentView = SharedMapping::map(object->get(), MiB, MapAccess::ReadWrite);
    REQUIRE(parentView.has_value());
    auto const helperView = SharedMapping::map(object->get(), MiB, MapAccess::ReadWrite);
    REQUIRE(helperView.has_value());

    REQUIRE(parentView->commit(0, GuestBytes).has_value());
    helperView->bytes().front() = std::byte { 0x5A };
    helperView->bytes()[GuestBytes - 1] = std::byte { 0xA5 };
    CHECK(parentView->bytes().front() == std::byte { 0x5A });
    CHECK(parentView->bytes()[GuestBytes - 1] == std::byte { 0xA5 });
}
#endif
