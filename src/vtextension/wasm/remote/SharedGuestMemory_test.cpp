// SPDX-License-Identifier: Apache-2.0
//
// The parent's side of memory a host shares: the object it makes per instance, the size it acknowledged, and
// how it judges a host's growth.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/SharedGuestMemory.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

using vtextension::ExtensionError;
using vtextension::wasm::MemoryFailure;
using vtextension::wasm::remote::GrowVerdict;
using vtextension::wasm::remote::SharedGuestMemory;

namespace proto = vtextension::wasm::remote::proto;

namespace
{

/// A guest page.
constexpr auto Page = proto::WasmPageBytes;

/// The cap the cases' memories are made under: 16 pages.
constexpr auto Cap = 16 * Page;

/// @return A memory of @p initialBytes under Cap, which must be made.
[[nodiscard]] SharedGuestMemory memoryOf(uint64_t initialBytes)
{
    auto memory = SharedGuestMemory::create(Cap, initialBytes);
    REQUIRE(memory.has_value());
    return *std::move(memory);
}

} // namespace

TEST_CASE("SharedGuestMemory.boundsAreContoursSize", "[vtextension][remote]")
{
    auto memory = memoryOf(Page);
    CHECK(memory.objectBytes() == proto::memoryObjectBytes(Cap));
    CHECK(memory.acknowledgedBytes() == Page);
    CHECK(memory.grant(7) == proto::MemoryGrant { .handle = 7, .objectBytes = Cap, .initialBytes = Page });

    // Whatever the object holds past the acknowledged size, no view reaches it.
    auto const last = memory.view(static_cast<uint32_t>(Page - 4), 4);
    REQUIRE(last.has_value());
    CHECK(last->size() == 4);
    CHECK(memory.view(static_cast<uint32_t>(Page - 4), 5) == std::unexpected(MemoryFailure::OutOfBounds));
    CHECK(memory.view(static_cast<uint32_t>(Page), 1) == std::unexpected(MemoryFailure::OutOfBounds));
    CHECK(memory.view(0xFFFF'FFFF, 1) == std::unexpected(MemoryFailure::OutOfBounds));
    CHECK(memory.view(0, 0xFFFF'FFFF) == std::unexpected(MemoryFailure::OutOfBounds));

    // Made under its cap, never past it, and of whole pages.
    CHECK(SharedGuestMemory::create(Cap, Cap + Page) == std::unexpected(ExtensionError::OutOfMemory));
    CHECK(SharedGuestMemory::create(Cap, Page + 1) == std::unexpected(ExtensionError::OutOfMemory));
    CHECK(SharedGuestMemory::create(Cap, 0).has_value());
}

TEST_CASE("SharedGuestMemory.growthIsChecked", "[vtextension][remote]")
{
    // A cap of 1 MiB: 16 pages.
    auto memory = memoryOf(Page);
    struct Ask
    {
        std::string_view what;
        uint64_t bytes;
        GrowVerdict verdict;
    };
    for (auto const& [what, bytes, verdict]: {
             Ask { .what = "not larger", .bytes = Page, .verdict = GrowVerdict::Violation },
             Ask { .what = "unaligned", .bytes = Page + 1, .verdict = GrowVerdict::Violation },
             Ask { .what = "past the cap", .bytes = 2 * Cap, .verdict = GrowVerdict::Violation },
             Ask { .what = "larger", .bytes = 2 * Page, .verdict = GrowVerdict::Granted },
             Ask { .what = "the same again", .bytes = 2 * Page, .verdict = GrowVerdict::Violation },
         })
    {
        INFO(what);
        CHECK(memory.grow(bytes) == verdict);
    }
    CHECK(memory.acknowledgedBytes() == 2 * Page);

    // Refused where no growth is granted, but judged alike: nothing is acknowledged.
    CHECK(memory.refuseGrowth(3 * Page) == GrowVerdict::Refused);
    CHECK(memory.refuseGrowth((3 * Page) + 1) == GrowVerdict::Violation);
    CHECK(memory.refuseGrowth(2 * Page) == GrowVerdict::Violation);
    CHECK(memory.acknowledgedBytes() == 2 * Page);

    // Up to the cap exactly, and not a page further.
    CHECK(memory.grow(Cap) == GrowVerdict::Granted);
    CHECK(memory.grow(Cap + Page) == GrowVerdict::Violation);
}

TEST_CASE("SharedGuestMemory.viewsSurviveGrowth", "[vtextension][remote]")
{
    auto memory = memoryOf(Page);
    auto const before = memory.view(16, 4);
    REQUIRE(before.has_value());
    (*before)[0] = std::byte { 0x5A };

    REQUIRE(memory.grow(4 * Page) == GrowVerdict::Granted);
    auto const after = memory.view(16, static_cast<uint32_t>((4 * Page) - 16));
    REQUIRE(after.has_value());
    // The memory did not move: the earlier view still names the same bytes, and the new ones are usable.
    CHECK(after->data() == before->data());
    CHECK((*after)[0] == std::byte { 0x5A });
    after->back() = std::byte { 0xA5 };
    CHECK(memory.view(static_cast<uint32_t>((4 * Page) - 1), 1).value()[0] == std::byte { 0xA5 });

    // Moving the memory keeps the mapping, and so every view of it.
    auto const moved = std::move(memory);
    CHECK(moved.view(16, 1).value().data() == before->data());
    CHECK(moved.acknowledgedBytes() == 4 * Page);
}

TEST_CASE("SharedGuestMemory.objectIsClosedOnceGranted", "[vtextension][remote]")
{
    auto memory = memoryOf(Page);
    CHECK(memory.object() != vtextension::wasm::remote::transport::NoNativeHandle);
    memory.closeObject();
    CHECK(memory.object() == vtextension::wasm::remote::transport::NoNativeHandle);
    // The mapping, and so the memory, lives on.
    auto const bytes = memory.view(0, 4);
    REQUIRE(bytes.has_value());
    (*bytes)[3] = std::byte { 1 };
    CHECK(memory.view(3, 1).value()[0] == std::byte { 1 });
}

#ifdef _WIN32
TEST_CASE("SharedGuestMemory.commitFollowsAcknowledgement", "[vtextension][remote]")
{
    // The object is reserved; the parent commits through its own view exactly what it acknowledged.
    auto memory = memoryOf(Page);
    auto const stateAt = [&](uint64_t offset) {
        auto const base = memory.view(0, 1).value().data();
        auto info = MEMORY_BASIC_INFORMATION {};
        REQUIRE(VirtualQuery(base + offset, &info, sizeof(info)) == sizeof(info));
        return info.State;
    };
    CHECK(stateAt(0) == MEM_COMMIT);
    CHECK(stateAt(Page - 1) == MEM_COMMIT);
    CHECK(stateAt(Page) == MEM_RESERVE);

    REQUIRE(memory.grow(3 * Page) == GrowVerdict::Granted);
    CHECK(stateAt((3 * Page) - 1) == MEM_COMMIT);
    CHECK(stateAt(3 * Page) == MEM_RESERVE);
}
#endif
