// SPDX-License-Identifier: Apache-2.0
//
// The extension host's mapping of guest memory: only the bytes the guest's memory spans are open, the object
// and the guard after it are not, what is open is the parent's object, an object of another size is refused,
// and opening never closes anything.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>
#include <vtextension/wasm/testing/Wine.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#elifdef __linux__
    #include <fstream>
    #include <string>
#endif

using vtextension::wasm::remote::transport::canMapGuestMemory;
using vtextension::wasm::remote::transport::createSharedObject;
using vtextension::wasm::remote::transport::GuestMemoryMapping;
using vtextension::wasm::remote::transport::isGuestMemoryLayout;
using vtextension::wasm::remote::transport::MapAccess;
using vtextension::wasm::remote::transport::MappingError;
using vtextension::wasm::remote::transport::SharedMapping;
using vtextension::wasm::remote::transport::SharedObjectUse;

namespace
{

/// A guest page.
constexpr auto Page = uint64_t { 64 } * 1024;

/// The object the cases map: four guest pages.
constexpr auto ObjectBytes = 4 * Page;

/// The guard the cases reserve after it.
constexpr auto GuardBytes = 2 * Page;

/// Skips the case where this system cannot map guest memory: only wine may say so.
void requireGuestMapping()
{
#ifdef _WIN32
    if (!canMapGuestMemory() && vtextension::wasm::testing::runsOnWine())
        SKIP("wine places no section view over a placeholder");
#endif
    REQUIRE(canMapGuestMemory());
}

#ifdef __linux__
/// @return The permissions /proc/self/maps shows for the mapping holding @p address ("rw-s", "---p", ...);
///         empty if none holds it.
[[nodiscard]] std::string permissionsAt(std::byte const* address)
{
    auto const where = reinterpret_cast<std::uintptr_t>(address);
    auto maps = std::ifstream { "/proc/self/maps" };
    auto line = std::string {};
    while (std::getline(maps, line))
    {
        auto const dash = line.find('-');
        auto const space = line.find(' ');
        if (dash == std::string::npos || space == std::string::npos)
            continue;
        auto const from = std::stoull(line.substr(0, dash), nullptr, 16);
        auto const to = std::stoull(line.substr(dash + 1, space - dash - 1), nullptr, 16);
        if (from <= where && where < to)
            return line.substr(space + 1, 4);
    }
    return {};
}
#endif

} // namespace

TEST_CASE("GuestMemoryMapping.layoutIsWholeGuestPages", "[vtextension]")
{
    CHECK(isGuestMemoryLayout(ObjectBytes, Page, GuardBytes));
    CHECK(isGuestMemoryLayout(ObjectBytes, 0, 0));
    CHECK(isGuestMemoryLayout(ObjectBytes, ObjectBytes, 0));
    CHECK_FALSE(isGuestMemoryLayout(0, 0, 0));
    CHECK_FALSE(isGuestMemoryLayout(ObjectBytes + 1, Page, 0));
    CHECK_FALSE(isGuestMemoryLayout(ObjectBytes, Page + 1, 0));
    CHECK_FALSE(isGuestMemoryLayout(ObjectBytes, Page, 1));
    CHECK_FALSE(isGuestMemoryLayout(ObjectBytes, ObjectBytes + Page, 0));
    CHECK_FALSE(isGuestMemoryLayout(ObjectBytes, Page, UINT64_MAX - (Page - 1)));
}

TEST_CASE("GuestMemoryMapping.opensOnlyTheInitialBytes", "[vtextension]")
{
    requireGuestMapping();
    auto const object = createSharedObject(ObjectBytes, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto const mapping = GuestMemoryMapping::map(object->get(), ObjectBytes, Page, GuardBytes);
    REQUIRE(mapping.has_value());
    CHECK(mapping->accessibleBytes() == Page);
    CHECK(mapping->objectBytes() == ObjectBytes);
    CHECK(mapping->guardBytes() == GuardBytes);
    auto const* const base = mapping->base();
    REQUIRE(base != nullptr);

#ifdef __linux__
    // Open and shared; closed and shared; the guard, closed and the helper's own.
    CHECK(permissionsAt(base) == "rw-s");
    CHECK(permissionsAt(base + Page - 1) == "rw-s");
    CHECK(permissionsAt(base + Page) == "---s");
    CHECK(permissionsAt(base + ObjectBytes - 1) == "---s");
    CHECK(permissionsAt(base + ObjectBytes) == "---p");
    CHECK(permissionsAt(base + ObjectBytes + GuardBytes - 1) == "---p");
#elifdef _WIN32
    auto region = MEMORY_BASIC_INFORMATION {};
    REQUIRE(::VirtualQuery(base, &region, sizeof(region)) != 0);
    CHECK(region.State == MEM_COMMIT);
    CHECK(region.Protect == PAGE_READWRITE);
    CHECK(region.Type == MEM_MAPPED);
    CHECK(region.RegionSize == Page);
    REQUIRE(::VirtualQuery(base + Page, &region, sizeof(region)) != 0);
    CHECK(region.State == MEM_RESERVE);
    CHECK(region.Type == MEM_MAPPED);
    REQUIRE(::VirtualQuery(base + ObjectBytes, &region, sizeof(region)) != 0);
    CHECK(region.State == MEM_RESERVE);
    CHECK(region.Type == MEM_PRIVATE);
    CHECK(region.RegionSize == GuardBytes);
#else
    SKIP("this system's mappings are not inspected");
#endif
}

TEST_CASE("GuestMemoryMapping.openedBytesAreTheParentsBytes", "[vtextension]")
{
    requireGuestMapping();
    auto const object = createSharedObject(ObjectBytes, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto parent = SharedMapping::map(object->get(), ObjectBytes, MapAccess::ReadWrite);
    REQUIRE(parent.has_value());
    // Contour commits what it grants through its own view, as the helper does through its.
    REQUIRE(parent->commit(0, ObjectBytes).has_value());
    auto helper = GuestMemoryMapping::map(object->get(), ObjectBytes, Page, GuardBytes);
    REQUIRE(helper.has_value());

    helper->base()[16] = std::byte { 42 };
    CHECK(parent->bytes()[16] == std::byte { 42 });
    parent->bytes()[Page - 1] = std::byte { 7 };
    CHECK(helper->base()[Page - 1] == std::byte { 7 });

    REQUIRE(helper->open(2 * Page).has_value());
    CHECK(helper->accessibleBytes() == 2 * Page);
    helper->base()[Page + 8] = std::byte { 9 };
    CHECK(parent->bytes()[Page + 8] == std::byte { 9 });
}

#ifdef _WIN32
TEST_CASE("GuestMemoryMapping.closedPagesAreThoseNoViewCommitted", "[vtextension]")
{
    // Windows commits a reserved section's pages for the section: the helper's closed pages stay closed only
    // while the parent commits no more than it granted, which is why SharedGuestMemory commits exactly that.
    if (vtextension::wasm::testing::runsOnWine())
        SKIP("wine commits a reserved section per view, unlike Windows");
    requireGuestMapping();
    auto const object = createSharedObject(ObjectBytes, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto parent = SharedMapping::map(object->get(), ObjectBytes, MapAccess::ReadWrite);
    REQUIRE(parent.has_value());
    REQUIRE(parent->commit(0, Page).has_value());
    auto const helper = GuestMemoryMapping::map(object->get(), ObjectBytes, Page, GuardBytes);
    REQUIRE(helper.has_value());
    auto const stateAt = [&](uint64_t offset) {
        auto region = MEMORY_BASIC_INFORMATION {};
        REQUIRE(::VirtualQuery(helper->base() + offset, &region, sizeof(region)) != 0);
        return region.State;
    };
    CHECK(stateAt(Page) == MEM_RESERVE);

    REQUIRE(parent->commit(Page, 2 * Page).has_value());
    CHECK(stateAt(Page) == MEM_COMMIT);
    CHECK(stateAt(2 * Page) == MEM_RESERVE);
}
#endif

TEST_CASE("GuestMemoryMapping.refusesAnObjectTooSmall", "[vtextension]")
{
    requireGuestMapping();
    auto const object = createSharedObject(ObjectBytes / 2, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto const mapping = GuestMemoryMapping::map(object->get(), ObjectBytes, Page, GuardBytes);
    REQUIRE_FALSE(mapping.has_value());
#ifndef _WIN32
    // POSIX measures the object; Windows only fails to map a view larger than the section.
    CHECK(mapping.error() == MappingError::WrongSize);
#endif

    // Sizes that are no layout are refused before the object is looked at.
    CHECK(GuestMemoryMapping::map(object->get(), ObjectBytes / 2, Page + 1, 0).error()
          == MappingError::WrongSize);
}

TEST_CASE("GuestMemoryMapping.neverShrinks", "[vtextension]")
{
    requireGuestMapping();
    auto const object = createSharedObject(ObjectBytes, SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto mapping = GuestMemoryMapping::map(object->get(), ObjectBytes, Page, 0);
    REQUIRE(mapping.has_value());

    REQUIRE(mapping->open(3 * Page).has_value());
    CHECK(mapping->accessibleBytes() == 3 * Page);
    REQUIRE(mapping->open(Page).has_value());
    CHECK(mapping->accessibleBytes() == 3 * Page);
    mapping->base()[(3 * Page) - 1] = std::byte { 1 };
    CHECK(mapping->base()[(3 * Page) - 1] == std::byte { 1 });

    CHECK(mapping->open(ObjectBytes + Page).error() == MappingError::WrongSize);
    CHECK(mapping->open((3 * Page) + 1).error() == MappingError::WrongSize);
    CHECK(mapping->accessibleBytes() == 3 * Page);
}

#ifdef _WIN32
TEST_CASE("GuestMemoryMapping.placeholdersWork", "[vtextension]")
{
    if (!canMapGuestMemory() && vtextension::wasm::testing::runsOnWine())
        SKIP("wine places no section view over a placeholder: the helper copies memory there");
    CHECK(canMapGuestMemory());
}
#endif
