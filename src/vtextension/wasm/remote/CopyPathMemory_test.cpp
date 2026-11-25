// SPDX-License-Identifier: Apache-2.0
//
// The parent's copies of an instance's memory: what is fetched, and what is written back.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <vtextension/wasm/remote/CopyPathMemory.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

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
    explicit FakeMemory(std::size_t size)
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, size))
            _bytes.push_back(static_cast<std::byte>(index & 0xFF));
    }

    /// @return A CopyPathMemory over this memory, holding at most @p capBytes.
    [[nodiscard]] CopyPathMemory copies(std::size_t capBytes)
    {
        return CopyPathMemory { InstanceId,
                                capBytes,
                                [this](uint32_t address,
                                       uint32_t size) -> std::optional<std::vector<std::byte>> {
                                    ++fetches;
                                    if (std::size_t { address } + size > _bytes.size())
                                        return std::nullopt;
                                    auto const range = std::span { _bytes }.subspan(address, size);
                                    return std::vector<std::byte> { range.begin(), range.end() };
                                } };
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

    int fetches = 0; ///< How often a range was fetched.

  private:
    std::vector<std::byte> _bytes;
};

} // namespace

TEST_CASE("CopyPathMemory.flushEmitsOnlyChangedRuns", "[vtextension][remote]")
{
    auto memory = FakeMemory { 256 };
    auto copies = memory.copies(1024);
    auto const view = copies.view(32, 16);
    REQUIRE(view.has_value());
    CHECK(view->size() == 16);
    CHECK(view->front() == std::byte { 32 });
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
}

TEST_CASE("CopyPathMemory.viewsInsideAFetchedRangeFetchNothing", "[vtextension][remote]")
{
    auto memory = FakeMemory { 256 };
    auto copies = memory.copies(1024);
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

TEST_CASE("CopyPathMemory.laterViewWins", "[vtextension][remote]")
{
    auto memory = FakeMemory { 256 };
    auto copies = memory.copies(1024);

    // A = [0, 8) is written before B = [4, 12) is fetched: B starts out with A's write.
    auto const a = copies.view(0, 8);
    REQUIRE(a.has_value());
    (*a)[5] = std::byte { 0xA5 };
    (*a)[1] = std::byte { 0xA1 };
    auto const b = copies.view(4, 8);
    REQUIRE(b.has_value());
    CHECK((*b)[1] == std::byte { 0xA5 });

    // Both write the byte they share; the later fetched one's write lands last.
    (*b)[1] = std::byte { 0xB5 };
    (*b)[6] = std::byte { 0xBA };
    memory.apply(copies.flush());
    copies.clear();

    CHECK(memory.at(1) == std::byte { 0xA1 });
    CHECK(memory.at(5) == std::byte { 0xB5 });
    CHECK(memory.at(10) == std::byte { 0xBA });
}

TEST_CASE("CopyPathMemory.refusesWhatItCannotHold", "[vtextension][remote]")
{
    auto memory = FakeMemory { 256 };
    auto copies = memory.copies(64);

    CHECK_FALSE(copies.view(250, 16).has_value());         // past the memory's end
    CHECK_FALSE(copies.view(0xFFFF'FFF0, 32).has_value()); // past the address space; fetches nothing
    CHECK(memory.fetches == 1);

    REQUIRE(copies.view(0, 48).has_value());
    CHECK_FALSE(copies.view(100, 17).has_value()); // past the cap: 48 + 17 > 64; fetches nothing
    CHECK(memory.fetches == 2);
    CHECK(copies.view(100, 16).has_value());
}
