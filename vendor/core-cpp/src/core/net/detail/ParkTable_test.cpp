// SPDX-License-Identifier: Apache-2.0
//
// `detail::ParkMap`, the open-addressing table the park table keys its parks in, and the park
// recycling beside it.
//
// The map replaced a `std::unordered_map` on the once-per-operation path, so it has to agree with
// one exactly -- including on the id that is never a key. `ParkId::invalid()` is zero, zero is the
// map's empty-slot marker, and `unregisterPark(ParkId::invalid())` is a documented no-op that real
// callers make. A first version matched zero against the first empty slot on the probe, counted a
// removal and shifted live entries out of their runs: a park then vanished from the table while its
// flow still waited, and a TLS case hung on a timer nobody could fire.
#include <core/async/ParkedWork.hpp>
#include <core/net/detail/ParkTable.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

using core::net::ParkId;
using core::net::ParkWake;
using core::net::detail::HandleWatch;
using core::net::detail::Park;
using core::net::detail::ParkMap;
using core::net::detail::ParkTable;
using core::net::testing::TestLoop;
using core::platform::ManualClock;

namespace
{
/// A handle other than `InvalidHandle`, however the platform spells one; never used as one.
template <typename Handle = core::platform::NativeHandle>
Handle someHandle()
{
    if constexpr (std::is_pointer_v<Handle>)
        return reinterpret_cast<Handle>(std::intptr_t { 7 });
    else
        return Handle { 7 };
}

/// SplitMix64: the same sequence on every run and every standard library, which is the point here.
/// A failure names a step, and the step has to be reproducible to be worth naming.
struct SplitMix64
{
    std::uint64_t state = 0;

    std::uint64_t operator()() noexcept
    {
        auto z = (state += 0x9E37'79B9'7F4A'7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31U);
    }
};
} // namespace

TEST_CASE("ParkMap refuses the zero id without touching what it holds", "[net][parktable]")
{
    auto map = ParkMap {};
    auto held = std::make_unique<Park>();
    auto* const raw = held.get();
    map.insert(ParkId { 1 }, std::move(held));

    CHECK(map.erase(ParkId::invalid()) == nullptr);
    CHECK(map.size() == 1);
    CHECK(map.find(ParkId { 1 }) == raw);
    CHECK(map.find(ParkId::invalid()) == nullptr);
}

TEST_CASE("ParkMap agrees with an unordered_map over a long random run", "[net][parktable]")
{
    // Few live parks and many ids, which is a loop's shape: an id is filed and taken per operation
    // while only a handful are live at once, so every removal shifts a short run and the probe
    // sequences cross the end of the array again and again.
    auto map = ParkMap {};
    auto reference = std::unordered_map<std::uint64_t, Park*> {};
    auto live = std::vector<std::uint64_t> {};
    auto rng = SplitMix64 { .state = 1729 };
    auto next = std::uint64_t { 0 };
    for ([[maybe_unused]] auto const step: std::views::iota(0, 200'000))
    {
        auto const choice = live.size() > 12 ? 1 + (rng() % 3) : rng() % 4;
        if (choice == 0 || live.empty())
        {
            auto park = std::make_unique<Park>();
            reference.emplace(++next, park.get());
            map.insert(ParkId { next }, std::move(park));
            live.push_back(next);
        }
        else if (choice == 1)
        {
            // Narrowed explicitly: `std::size_t` is 32 bits under WebAssembly.
            auto const index = static_cast<std::size_t>(rng() % live.size());
            auto const id = live[index];
            auto const taken = map.erase(ParkId { id });
            REQUIRE(taken.get() == reference.at(id));
            reference.erase(id);
            live[index] = live.back();
            live.pop_back();
        }
        else if (choice == 2)
        {
            // An id that is not held -- zero, one never filed, or one already taken -- removes
            // nothing.
            auto const absent = rng() % 3 == 0 ? std::uint64_t { 0 } : next + 1 + (rng() % 8);
            REQUIRE(map.erase(ParkId { absent }) == nullptr);
        }
        else
        {
            auto const id = rng() % (next + 4);
            auto const found = reference.find(id);
            REQUIRE(map.find(ParkId { id }) == (found == reference.end() ? nullptr : found->second));
        }
        REQUIRE(map.size() == reference.size());
    }
}

TEST_CASE("ParkTable hands a recycled park back reset", "[net][parktable]")
{
    // Every field but `parked` set to something other than its default: `recycle` resets a park
    // field by field, and a field it forgets reaches the next operation's park.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto table = ParkTable {};
    auto watch = HandleWatch {};
    auto park = table.acquire();
    park->handler.owner = &table;
    park->loop = &loop;
    park->handle = someHandle();
    park->waiterKey = &table;
    park->attached = true;
    park->watch = &watch;
    park->onExpired = [](void*) {
    };
    park->onReady = [](void*, ParkWake) {
    };
    park->callbackState = &table;
    park->ownedByLoop = true;
    park->readinessQueued = true;
    park->queuedWake = ParkWake::Abandoned;
    park->deadline = clock.now() + std::chrono::seconds { 1 };
    park->residentSlot = 5;
    auto const id = table.add(std::move(park));
    auto taken = table.take(id);
    REQUIRE(taken != nullptr);
    // The two the table writes itself, set after it is done with them.
    taken->sequence = 3;
    taken->handleIndexed = true;
    auto* const raw = taken.get();

    table.recycle(std::move(taken));
    auto again = table.acquire();
    CHECK(again.get() == raw); // reused rather than reallocated
    CHECK(again->id == ParkId::invalid());
    CHECK(!again->parked);
    CHECK(again->handler.owner == nullptr);
    CHECK(again->loop == nullptr);
    CHECK(again->handle == core::platform::InvalidHandle);
    CHECK(again->waiterKey == nullptr);
    CHECK(!again->attached);
    CHECK(again->watch == nullptr);
    CHECK(!again->handleIndexed);
    CHECK(again->onExpired == nullptr);
    CHECK(again->onReady == nullptr);
    CHECK(again->callbackState == nullptr);
    CHECK(!again->ownedByLoop);
    CHECK(!again->readinessQueued);
    CHECK(again->queuedWake == ParkWake::Ready);
    CHECK(!again->deadline.has_value());
    CHECK(again->sequence == 0);
    CHECK(again->residentSlot == ParkTable::NoResidentSlot);
}

TEST_CASE("ParkTable does not hand out again a park recycled while it still holds work", "[net][parktable]")
{
    // `recycle` is told a park is empty; one that is not must be freed rather than kept, or the next
    // operation's park would arrive holding the previous one's coroutine. The work here is a handle
    // with no claim, so freeing it frees nothing further.
    auto table = ParkTable {};
    auto park = table.acquire();
    park->parked = core::async::detail::Parked { core::async::ParkedWork { .resume = std::noop_coroutine(),
                                                                           .abandon = {} } };
    REQUIRE(park->parked);

    table.recycle(std::move(park));
    auto again = table.acquire();
    CHECK_FALSE(again->parked);
}

namespace
{
/// Fills @p park in as `EventLoop::registerPark` fills a socket operation's: frameless, on a
/// handle's watch.
/// @param park The resident park to fill.
/// @param watch The watch it takes a slot on.
/// @param state What its callback is handed.
void fillAsOperation(Park& park, HandleWatch& watch, void* state)
{
    park.handle = someHandle();
    park.onReady = [](void*, ParkWake) {
    };
    park.callbackState = state;
    park.watch = &watch;
}

/// @return Whether @p ids holds @p id.
bool holds(std::vector<ParkId> const& ids, ParkId id)
{
    return std::ranges::find(ids, id) != ids.end();
}
} // namespace

TEST_CASE("A resident park gets an id per operation, and a retired id finds nothing",
          "[net][parktable][resident]")
{
    auto table = ParkTable {};
    auto watch = HandleWatch {};
    auto const slot = table.openResident();
    REQUIRE(slot != ParkTable::NoResidentSlot);

    auto* const park = table.idleResident(slot);
    REQUIRE(park != nullptr);
    fillAsOperation(*park, watch, &table);
    auto const first = table.addResident(slot);
    CHECK(ParkTable::isResident(first));
    CHECK(table.find(first) == park);
    CHECK(table.idleResident(slot) == nullptr); // an operation holds it
    CHECK(table.readinessCount() == 1);
    CHECK(table.size() == 1);
    CHECK(holds(table.ids(), first));
    CHECK(table.take(first) == nullptr); // retired, never taken

    table.retireResident(*park);
    CHECK(table.find(first) == nullptr);
    CHECK(table.readinessCount() == 0); // an idle park is not counted
    CHECK(table.size() == 0);
    CHECK(table.ids().empty());

    // The next operation: the same storage, a new name, and the old name still finds nothing.
    auto* const again = table.idleResident(slot);
    CHECK(again == park);
    fillAsOperation(*again, watch, &table);
    auto const second = table.addResident(slot);
    CHECK(second != first);
    CHECK(table.find(first) == nullptr);
    CHECK(table.find(second) == park);
    table.retireResident(*again);
}

TEST_CASE("A retired resident park keeps nothing of its operation", "[net][parktable][resident]")
{
    auto table = ParkTable {};
    auto watch = HandleWatch {};
    auto const slot = table.openResident();
    auto* const park = table.idleResident(slot);
    REQUIRE(park != nullptr);
    fillAsOperation(*park, watch, &table);
    std::ignore = table.addResident(slot);
    park->readinessQueued = true;
    park->queuedWake = ParkWake::Abandoned;

    table.retireResident(*park);
    CHECK(park->id == ParkId::invalid());
    CHECK(park->handle == core::platform::InvalidHandle);
    CHECK(park->onReady == nullptr);
    CHECK(park->callbackState == nullptr);
    CHECK(park->watch == nullptr);
    CHECK(!park->readinessQueued);
    CHECK(park->queuedWake == ParkWake::Ready);
    CHECK(!park->handleIndexed);
    CHECK(park->residentSlot == slot); // still the slot's
}

TEST_CASE("A resident slot closed while idle is reopened, and never repeats an id",
          "[net][parktable][resident]")
{
    auto table = ParkTable {};
    auto watch = HandleWatch {};
    auto seen = std::vector<ParkId> {};
    auto slot = table.openResident();
    for ([[maybe_unused]] auto const handle: std::views::iota(0, 3))
    {
        for ([[maybe_unused]] auto const operation: std::views::iota(0, 4))
        {
            auto* const park = table.idleResident(slot);
            REQUIRE(park != nullptr);
            fillAsOperation(*park, watch, &table);
            auto const id = table.addResident(slot);
            CHECK(!holds(seen, id));
            seen.push_back(id);
            table.retireResident(*park);
        }
        table.closeResident(slot);
        auto const reopened = table.openResident();
        CHECK(reopened == slot); // the freed slot is handed out again
        slot = reopened;
    }
    for (auto const id: seen)
        CHECK(table.find(id) == nullptr);
    table.closeResident(slot);
}

TEST_CASE("A resident slot closed under a filed operation is freed when that operation retires",
          "[net][parktable][resident]")
{
    auto table = ParkTable {};
    auto watch = HandleWatch {};
    auto const slot = table.openResident();
    auto* const park = table.idleResident(slot);
    REQUIRE(park != nullptr);
    fillAsOperation(*park, watch, &table);
    auto const filed = table.addResident(slot);

    // The handle closes with its read still parked: the park outlives its watch until its owner
    // takes it, and the slot is not handed out meanwhile.
    park->watch = nullptr;
    table.closeResident(slot);
    CHECK(table.find(filed) == park);
    CHECK(table.readinessCount() == 1);
    auto const other = table.openResident();
    CHECK(other != slot);

    table.retireResident(*park);
    CHECK(table.find(filed) == nullptr);
    CHECK(table.readinessCount() == 0);
    CHECK(table.openResident() == slot); // freed by the retire
    table.closeResident(slot);
    table.closeResident(other);
}

TEST_CASE("takeAll hands out the resident parks an operation holds, and only those",
          "[net][parktable][resident]")
{
    auto table = ParkTable {};
    auto watch = HandleWatch {};
    auto const busy = table.openResident();
    auto const idle = table.openResident();
    auto* const park = table.idleResident(busy);
    REQUIRE(park != nullptr);
    fillAsOperation(*park, watch, &table);
    auto const filed = table.addResident(busy);
    REQUIRE(table.idleResident(idle) != nullptr);
    auto const ordinary = table.add(table.acquire());
    CHECK(!ParkTable::isResident(ordinary));

    auto const taken = table.takeAll();
    CHECK(taken.size() == 2);
    CHECK(std::ranges::any_of(taken, [park](auto const& each) { return each.get() == park; }));
    CHECK(table.size() == 0);
    CHECK(table.readinessCount() == 0);
    CHECK(table.find(filed) == nullptr);

    // A slot a watch still holds keeps working after the sweep, under a name never used before.
    auto* const fresh = table.idleResident(busy);
    REQUIRE(fresh != nullptr);
    fillAsOperation(*fresh, watch, &table);
    auto const next = table.addResident(busy);
    CHECK(next != filed);
    CHECK(table.find(next) == fresh);
    table.retireResident(*fresh);
}

TEST_CASE("Resident slots freed after a burst keep no more parks than the spare list holds",
          "[net][parktable][resident]")
{
    // A burst of connections, each direction parking once, then closing: the loop must not hold a
    // park per direction it ever had. Freed slots hand their parks to the spare list, whose cap is
    // what the loop keeps; the slots keep their generations.
    constexpr auto Burst = 500;
    auto table = ParkTable {};
    auto watch = HandleWatch {};
    auto slots = std::vector<std::uint32_t> {};
    auto issued = std::vector<ParkId> {};
    for ([[maybe_unused]] auto const direction: std::views::iota(0, Burst))
    {
        auto const slot = table.openResident();
        auto* const park = table.idleResident(slot);
        REQUIRE(park != nullptr);
        fillAsOperation(*park, watch, &table);
        issued.push_back(table.addResident(slot));
        table.retireResident(*park);
        slots.push_back(slot);
    }
    CHECK(table.retainedParkCount() == std::size_t { Burst }); // idle, each still its slot's
    for (auto const slot: slots)
        table.closeResident(slot);
    auto const kept = table.retainedParkCount();
    CHECK(kept <= ParkTable::MaxSpareParks);

    // Steady churn below the cap draws from the spares: opening and parking again makes no park.
    for ([[maybe_unused]] auto const direction: std::views::iota(0, 16))
    {
        auto const slot = table.openResident();
        auto* const park = table.idleResident(slot);
        REQUIRE(park != nullptr);
        fillAsOperation(*park, watch, &table);
        auto const id = table.addResident(slot);
        CHECK(!holds(issued, id)); // the slot's generation outlived its park
        table.retireResident(*park);
        table.closeResident(slot);
    }
    CHECK(table.retainedParkCount() == kept);
}

TEST_CASE("A resident slot whose generations are spent is retired, never wrapped",
          "[net][parktable][resident]")
{
    auto table = ParkTable {};
    auto watch = HandleWatch {};
    auto const slot = table.openResident();
    table.setResidentGenerationForTesting(slot, ParkTable::MaxResidentGeneration - 1);

    // The last generation the slot has.
    auto* const park = table.idleResident(slot);
    REQUIRE(park != nullptr);
    fillAsOperation(*park, watch, &table);
    auto const last = table.addResident(slot);
    CHECK(ParkTable::isResident(last));
    CHECK(table.find(last) == park);
    table.retireResident(*park);

    // Spent: no further operation is filed in it, so the loop takes the ordinary path instead.
    CHECK(table.idleResident(slot) == nullptr);
    CHECK(table.find(last) == nullptr);

    // And closing it never hands it out again, however many slots are opened after.
    table.closeResident(slot);
    for ([[maybe_unused]] auto const other: std::views::iota(0, 8))
        CHECK(table.openResident() != slot);
}
