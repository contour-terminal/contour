// SPDX-License-Identifier: Apache-2.0
//
// The parent's copies of an instance's memory: what is fetched, and what is written back.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <vtextension/wasm/remote/CopyPathMemory.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

using vtextension::wasm::MemoryFailure;
using vtextension::wasm::remote::CopyPathMemory;

namespace proto = vtextension::wasm::remote::proto;

namespace
{

/// The instance the tests' writes are addressed to.
constexpr auto InstanceId = uint32_t { 7 };

/// An instance's memory, as its host holds it: every fetch reads it, every write lands in it.
class FakeMemory
{
  public:
    /// @param size The memory's size in bytes; byte i holds i, modulo 256.
    explicit FakeMemory(std::size_t size = 3 * CopyPathMemory::PageBytes)
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, size))
            _bytes.push_back(static_cast<std::byte>(index & 0xFF));
    }

    /// @return A CopyPathMemory over this memory, as large as it may grow.
    [[nodiscard]] CopyPathMemory copies()
    {
        return CopyPathMemory {
            InstanceId,
            _bytes.size(),
            [this](uint32_t address, uint32_t size) -> std::expected<std::vector<std::byte>, MemoryFailure> {
                ++fetches;
                reads.push_back(Read { .address = address, .size = size });
                if (std::size_t { address } + size > _bytes.size())
                    return std::unexpected(MemoryFailure::OutOfBounds);
                auto const range = std::span { _bytes }.subspan(address, size);
                return std::vector<std::byte> { range.begin(), range.end() };
            }
        };
    }

    /// Applies @p writes in order.
    void apply(std::vector<proto::MemoryWrite> const& writes)
    {
        for (auto const& write: writes)
        {
            REQUIRE(write.instanceId == InstanceId);
            REQUIRE(std::size_t { write.address } + write.bytes.size() <= _bytes.size());
            std::ranges::copy(write.bytes, _bytes.begin() + write.address);
        }
    }

    /// @return The byte at @p address.
    [[nodiscard]] std::byte at(std::size_t address) const { return _bytes.at(address); }

    /// One range read from the memory.
    struct Read
    {
        uint32_t address = 0;
        uint32_t size = 0;
        bool operator==(Read const&) const = default;
    };

    int fetches = 0;         ///< How often a range was fetched.
    std::vector<Read> reads; ///< Every range fetched, in order.

  private:
    std::vector<std::byte> _bytes;
};

} // namespace

/// The size of a page of the shadow.
constexpr auto Page = static_cast<uint32_t>(CopyPathMemory::PageBytes);

TEST_CASE("CopyPathMemory.flushEmitsOnlyChangedRuns", "[vtextension][remote]")
{
    auto memory = FakeMemory {};
    auto copies = memory.copies();
    auto const view = copies.view(32, 16);
    REQUIRE(view.has_value());
    CHECK(view->size() == 16);
    CHECK(view->front() == std::byte { 32 });
    CHECK(memory.reads == std::vector { FakeMemory::Read { .address = 0, .size = Page } });
    CHECK(copies.flush().empty());

    (*view)[3] = std::byte { 0xAA };
    (*view)[4] = std::byte { 0xBB };
    // A byte written back to what it held is no change.
    (*view)[9] = std::byte { 0xCC };
    (*view)[9] = std::byte { 32 + 9 };

    CHECK(copies.flush()
          == std::vector { proto::MemoryWrite { .instanceId = InstanceId,
                                                .address = 35,
                                                .bytes = { std::byte { 0xAA }, std::byte { 0xBB } } } });
    // What was flushed counts as unchanged from then on.
    CHECK(copies.flush().empty());
}

TEST_CASE("CopyPathMemory.viewsOfHeldPagesFetchNothing", "[vtextension][remote]")
{
    auto memory = FakeMemory {};
    auto copies = memory.copies();
    auto const whole = copies.view(0, 64);
    REQUIRE(whole.has_value());
    (*whole)[10] = std::byte { 0xEE };
    auto const inner = copies.view(8, 4);
    REQUIRE(inner.has_value());
    CHECK((*inner)[2] == std::byte { 0xEE });
    CHECK(memory.fetches == 1);

    auto const empty = copies.view(1000, 0);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
    CHECK(memory.fetches == 1);

    copies.clear();
    REQUIRE(copies.view(8, 4).has_value());
    CHECK(memory.fetches == 2);
}

TEST_CASE("CopyPathMemory.overlappingViewsAreTheSameBytes", "[vtextension][remote]")
{
    auto memory = FakeMemory {};
    auto copies = memory.copies();

    // A = [0, 8), B = [4, 12): B's first byte is A's fifth, whichever writes it, and whenever.
    auto const a = copies.view(0, 8);
    REQUIRE(a.has_value());
    auto const b = copies.view(4, 8);
    REQUIRE(b.has_value());
    CHECK(&(*b)[1] == &(*a)[5]);
    (*b)[3] = std::byte { 0xB7 };
    CHECK((*a)[7] == std::byte { 0xB7 });
    (*a)[5] = std::byte { 0xA5 };
    CHECK((*b)[1] == std::byte { 0xA5 });

    // Views taken again see the latest write too, without fetching again.
    auto const again = copies.view(0, 8);
    REQUIRE(again.has_value());
    CHECK((*again)[5] == std::byte { 0xA5 });
    CHECK((*again)[7] == std::byte { 0xB7 });
    CHECK(memory.fetches == 1);

    memory.apply(copies.flush());
    CHECK(memory.at(5) == std::byte { 0xA5 });
    CHECK(memory.at(7) == std::byte { 0xB7 });
}

TEST_CASE("CopyPathMemory.noWriteIsLost", "[vtextension][remote]")
{
    auto memory = FakeMemory {};
    auto copies = memory.copies();

    // Two views write the same value to a byte they share, and then one of them writes another: the last
    // write is what the instance gets.
    auto const a = copies.view(0, 8);
    REQUIRE(a.has_value());
    auto const b = copies.view(4, 8);
    REQUIRE(b.has_value());
    (*b)[1] = std::byte { 0 };
    (*a)[5] = std::byte { 0 };
    auto const again = copies.view(0, 8);
    REQUIRE(again.has_value());
    (*again)[5] = std::byte { 9 };

    memory.apply(copies.flush());
    CHECK(memory.at(5) == std::byte { 9 });
}

TEST_CASE("CopyPathMemory.alternatingOverlappingViewsFetchOnce", "[vtextension][remote]")
{
    auto memory = FakeMemory {};
    auto copies = memory.copies();

    for (auto const round: std::views::iota(0, 64))
    {
        // Each writes where the other reaches too.
        auto const a = copies.view(0, 8);
        REQUIRE(a.has_value());
        (*a)[4 + (round % 4)] = static_cast<std::byte>(round);
        auto const b = copies.view(4, 8);
        REQUIRE(b.has_value());
        (*b)[round % 8] = static_cast<std::byte>(round);
    }
    CHECK(memory.fetches == 1);

    memory.apply(copies.flush());
    CHECK(memory.at(4 + 7) == std::byte { 63 });
    CHECK(memory.at(4 + 3) == std::byte { 63 });
    CHECK(memory.at(4 + 2) == std::byte { 62 });
}

TEST_CASE("CopyPathMemory.adjacentChangesFlushAsOneWrite", "[vtextension][remote]")
{
    auto memory = FakeMemory {};
    auto copies = memory.copies();

    // Written through two views that meet, at either side of where they meet, and across pages.
    auto const left = copies.view(Page - 4, 4);
    REQUIRE(left.has_value());
    auto const right = copies.view(Page, 4);
    REQUIRE(right.has_value());
    std::ranges::fill(*left, std::byte { 0x11 });
    std::ranges::fill(*right, std::byte { 0x22 });

    auto const writes = copies.flush();
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].address == Page - 4);
    CHECK(writes[0].bytes.size() == 8);
    memory.apply(writes);
    CHECK(memory.at(Page - 1) == std::byte { 0x11 });
    CHECK(memory.at(Page) == std::byte { 0x22 });
}

TEST_CASE("CopyPathMemory.viewAcrossPages", "[vtextension][remote]")
{
    auto memory = FakeMemory {};
    auto copies = memory.copies();

    // Absent pages next to each other are fetched in one read; a held page between splits the read.
    REQUIRE(copies.view(Page + 8, 4).has_value());
    auto const across = copies.view(Page - 2, 4);
    REQUIRE(across.has_value());
    CHECK((*across)[1] == static_cast<std::byte>((Page - 1) & 0xFF));
    CHECK((*across)[2] == std::byte { 0 });
    auto const whole = copies.view(0, 3 * Page);
    REQUIRE(whole.has_value());
    CHECK(memory.reads
          == std::vector { FakeMemory::Read { .address = Page, .size = Page },
                           FakeMemory::Read { .address = 0, .size = Page },
                           FakeMemory::Read { .address = 2 * Page, .size = Page } });

    copies.clear();
    REQUIRE(copies.view(Page - 2, 4).has_value());
    CHECK(memory.reads.back() == FakeMemory::Read { .address = 0, .size = 2 * Page });
}

TEST_CASE("CopyPathMemory.refusesWhatLiesPastTheMemory", "[vtextension][remote]")
{
    auto memory = FakeMemory {};
    auto copies = memory.copies();

    CHECK(copies.view((3 * Page) - 8, 16).error() == MemoryFailure::OutOfBounds); // past the memory's limit
    CHECK_FALSE(copies.view(0xFFFF'FFF0, 32).has_value());                        // past the address space
    CHECK_FALSE(copies.view(0xFFFF'FFFF, 0xFFFF'FFFF).has_value());
    CHECK(memory.fetches == 0);
    CHECK(copies.view((3 * Page) - 8, 8).has_value());

    // A memory smaller than its limit refuses what it does not hold: its host cannot read it.
    auto copiesOfSmall = CopyPathMemory { InstanceId, 2 * std::size_t { Page }, [](uint32_t, uint32_t) {
                                             return std::expected<std::vector<std::byte>, MemoryFailure> {
                                                 std::unexpect, MemoryFailure::OutOfBounds
                                             };
                                         } };
    CHECK(copiesOfSmall.view(Page, 4).error() == MemoryFailure::OutOfBounds);
}

TEST_CASE("CopyPathMemory.reportsAHostGoneAsSuch", "[vtextension][remote]")
{
    // A host that cannot be reached is not a range out of bounds: the caller blames the host, not the guest.
    auto copies = CopyPathMemory { InstanceId, 2 * std::size_t { Page }, [](uint32_t, uint32_t) {
                                      return std::expected<std::vector<std::byte>, MemoryFailure> {
                                          std::unexpect, MemoryFailure::HostCrashed
                                      };
                                  } };
    CHECK(copies.view(0, 4).error() == MemoryFailure::HostCrashed);
    CHECK(copies.view(Page - 2, 4).error() == MemoryFailure::HostCrashed);
}

TEST_CASE("CopyPathMemory.snapshotsOnlyTheFetchedPages", "[vtextension][remote]")
{
    auto memory = FakeMemory { 64 * std::size_t { Page } };
    auto copies = memory.copies();
    CHECK(copies.snapshotBytes() == 0);

    REQUIRE(copies.view(Page - 2, 4).has_value());
    REQUIRE(copies.view(40 * Page, 8).has_value());
    CHECK(copies.snapshotBytes() == 3 * std::size_t { Page });
    // A view of held pages adds none.
    REQUIRE(copies.view(0, 8).has_value());
    CHECK(copies.snapshotBytes() == 3 * std::size_t { Page });

    // Written and flushed, the snapshots stay what was fetched: the arena does not grow.
    auto const view = copies.view(Page - 2, 4);
    REQUIRE(view.has_value());
    std::ranges::fill(*view, std::byte { 0x5A });
    memory.apply(copies.flush());
    CHECK(copies.flush().empty());
    CHECK(copies.snapshotBytes() == 3 * std::size_t { Page });
    CHECK(memory.at(Page - 2) == std::byte { 0x5A });
    CHECK(memory.at(Page + 1) == std::byte { 0x5A });

    copies.clear();
    CHECK(copies.snapshotBytes() == 0);
    REQUIRE(copies.view(10 * Page, 4).has_value());
    CHECK(copies.snapshotBytes() == Page);
}
