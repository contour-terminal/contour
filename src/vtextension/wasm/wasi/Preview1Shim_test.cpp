// SPDX-License-Identifier: Apache-2.0
//
// The WASI preview 1 shim, function by function, against a plain 64 KiB memory and a deterministic
// environment; and the environment a process runs it with.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <vtextension/wasm/WasiAllowList.hpp>
#include <vtextension/wasm/wasi/Preview1Shim.hpp>
#include <vtextension/wasm/wasi/SystemEnvironment.hpp>

using vtextension::wasm::AllowedWasiFunctions;
using vtextension::wasm::wasi::ClockId;
using vtextension::wasm::wasi::Errno;
using vtextension::wasm::wasi::findShimFunction;
using vtextension::wasm::wasi::ShimEnvironment;
using vtextension::wasm::wasi::ShimFunction;
using vtextension::wasm::wasi::shimFunctions;
using vtextension::wasm::wasi::ShimOutcome;
using vtextension::wasm::wasi::ShimResult;
using vtextension::wasm::wasi::systemEnvironment;

namespace
{

/// The size of the guest memory every case runs against: one WebAssembly page.
constexpr auto MemoryBytes = uint32_t { 64 } * 1024;

/// A byte the memory is filled with, so that what a function leaves alone shows.
constexpr auto Untouched = std::byte { 0xEE };

/// The first byte the test environment's random source produces; each next one is one more.
constexpr auto FirstRandomByte = uint8_t { 0xA0 };

/// @return Nanoseconds from the test environment's clock @p clockId: 1000 plus its id.
[[nodiscard]] uint64_t testClock(uint32_t clockId)
{
    return 1000 + uint64_t { clockId };
}

/// Fills @p bytes with FirstRandomByte, FirstRandomByte + 1, and so on.
void testRandom(std::span<std::byte> bytes)
{
    auto next = FirstRandomByte;
    for (auto& byte: bytes)
        byte = std::byte { next++ };
}

/// @return An environment whose clocks and random source give the same values on every run.
[[nodiscard]] ShimEnvironment testEnvironment()
{
    return ShimEnvironment { .clock = &testClock, .random = &testRandom };
}

/// One guest memory, filled with Untouched, and the shim functions called on it.
class Guest
{
  public:
    /// Calls the shim function @p name with @p args.
    /// @return What it returned.
    [[nodiscard]] ShimResult call(std::string_view name, std::initializer_list<uint64_t> args)
    {
        auto const* const function = findShimFunction(name);
        REQUIRE(function != nullptr);
        auto const arguments = std::vector<uint64_t>(args);
        REQUIRE(arguments.size() == function->preview1.parameterCount);
        return function->call(_environment, _memory, arguments);
    }

    /// @return The errno of a call to @p name with @p args, which must have returned.
    [[nodiscard]] Errno errnoOf(std::string_view name, std::initializer_list<uint64_t> args)
    {
        auto const result = call(name, args);
        REQUIRE(result.outcome == ShimOutcome::Returned);
        return result.error;
    }

    /// @return The little-endian unsigned integer of @p size bytes at @p address.
    [[nodiscard]] uint64_t load(uint32_t address, uint32_t size) const
    {
        auto value = uint64_t { 0 };
        for (auto const offset: std::views::iota(uint32_t { 0 }, size) | std::views::reverse)
            value = (value << 8U) | std::to_integer<uint64_t>(_memory.at(address + offset));
        return value;
    }

    /// Stores @p value as a little-endian u32 at @p address.
    void store32(uint32_t address, uint32_t value)
    {
        for (auto const offset: std::views::iota(uint32_t { 0 }, uint32_t { 4 }))
            _memory.at(address + offset) = std::byte { static_cast<uint8_t>(value >> (offset * 8U)) };
    }

    /// @return Whether the @p size bytes at @p address are all still Untouched.
    [[nodiscard]] bool isUntouched(uint32_t address, uint32_t size) const
    {
        return std::ranges::all_of(std::span { _memory }.subspan(address, size),
                                   [](std::byte byte) { return byte == Untouched; });
    }

    /// @return Whether the whole memory is still Untouched.
    [[nodiscard]] bool isUntouched() const { return isUntouched(0, MemoryBytes); }

  private:
    ShimEnvironment _environment = testEnvironment();
    std::vector<std::byte> _memory = std::vector<std::byte>(MemoryBytes, Untouched);
};

/// The address the last two bytes of the memory start at: no u32 or u64 fits there.
constexpr auto NearEnd = uint64_t { MemoryBytes - 2 };

/// File descriptors 0 to 2, the standard streams.
constexpr auto StandardStreams = std::to_array<uint64_t>({ 0, 1, 2 });

/// The rights a standard stream has: fd_seek (bit 2) and fd_write (bit 6), the two the shim implements.
constexpr auto StandardStreamRights = uint64_t { 0b0100'0100 };

/// The filetype of a character device.
constexpr auto CharacterDevice = uint64_t { 2 };

} // namespace

TEST_CASE("Preview1Shim.tableMatchesAllowList")
{
    auto const functions = shimFunctions();
    REQUIRE(functions.size() == AllowedWasiFunctions.size());
    CHECK(std::ranges::equal(functions | std::views::transform([](ShimFunction const& function) {
                                 return function.preview1.name;
                             }),
                             AllowedWasiFunctions));
    for (auto const& function: functions)
    {
        INFO(function.preview1.name);
        CHECK(function.call != nullptr);
        CHECK(findShimFunction(function.preview1.name) == &function);
    }
    CHECK(findShimFunction("poll_oneoff") == nullptr);
}

TEST_CASE("Preview1Shim.args_get")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("args_get", { 100, 200 }) == Errno::Success);
    CHECK(guest.isUntouched()); // There is no argument to write.
}

TEST_CASE("Preview1Shim.args_sizes_get")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("args_sizes_get", { 100, 104 }) == Errno::Success);
    CHECK(guest.load(100, 4) == 0); // argc
    CHECK(guest.load(104, 4) == 0); // the size of the argument strings
    CHECK(guest.isUntouched(108, 4));
}

TEST_CASE("Preview1Shim.environ_get")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("environ_get", { 100, 200 }) == Errno::Success);
    CHECK(guest.isUntouched()); // There is no variable to write.
}

TEST_CASE("Preview1Shim.environ_sizes_get")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("environ_sizes_get", { 100, 104 }) == Errno::Success);
    CHECK(guest.load(100, 4) == 0); // the count of variables
    CHECK(guest.load(104, 4) == 0); // the size of their strings
    CHECK(guest.isUntouched(108, 4));
}

TEST_CASE("Preview1Shim.clock_time_get")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("clock_time_get", { 0, 1, 200 }) == Errno::Success);
    CHECK(guest.load(200, 8) == 1000); // the environment's realtime clock
    CHECK(guest.errnoOf("clock_time_get", { 1, 1, 208 }) == Errno::Success);
    CHECK(guest.load(208, 8) == 1001); // its monotonic clock
    CHECK(guest.isUntouched(216, 4));

    auto fresh = Guest {};
    // The CPU-time clocks are clocks of preview 1, but the shim keeps an instance's CPU time to itself.
    CHECK(fresh.errnoOf("clock_time_get", { 2, 1, 200 }) == Errno::Nosys);
    CHECK(fresh.errnoOf("clock_time_get", { 3, 1, 200 }) == Errno::Nosys);
    CHECK(fresh.errnoOf("clock_time_get", { 4, 1, 200 }) == Errno::Inval);
    CHECK(fresh.errnoOf("clock_time_get", { std::numeric_limits<uint32_t>::max(), 1, 200 }) == Errno::Inval);
    CHECK(fresh.isUntouched());
}

TEST_CASE("Preview1Shim.clock_res_get")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("clock_res_get", { 0, 200 }) == Errno::Success);
    CHECK(guest.load(200, 8) == 1);
    CHECK(guest.errnoOf("clock_res_get", { 1, 208 }) == Errno::Success);
    CHECK(guest.load(208, 8) == 1);

    auto fresh = Guest {};
    CHECK(fresh.errnoOf("clock_res_get", { 2, 200 }) == Errno::Nosys);
    CHECK(fresh.errnoOf("clock_res_get", { 3, 200 }) == Errno::Nosys);
    CHECK(fresh.errnoOf("clock_res_get", { 4, 200 }) == Errno::Inval);
    CHECK(fresh.isUntouched());
}

TEST_CASE("Preview1Shim.fd_write")
{
    auto guest = Guest {};
    // Two buffers, of 5 and 7 bytes; nothing reads what they hold.
    guest.store32(300, 400);
    guest.store32(304, 5);
    guest.store32(308, 500);
    guest.store32(312, 7);
    for (auto const fd: StandardStreams)
    {
        INFO("fd " << fd);
        CHECK(guest.errnoOf("fd_write", { fd, 300, 2, 320 }) == Errno::Success);
        CHECK(guest.load(320, 4) == 12);
    }
    CHECK(guest.errnoOf("fd_write", { 1, 300, 0, 324 }) == Errno::Success);
    CHECK(guest.load(324, 4) == 0);
    CHECK(guest.isUntouched(400, 5));
    CHECK(guest.isUntouched(500, 7));

    CHECK(guest.errnoOf("fd_write", { 3, 300, 2, 328 }) == Errno::Badf);
    CHECK(guest.isUntouched(328, 4));

    // A buffer reaching past the end of the memory.
    guest.store32(304, MemoryBytes);
    CHECK(guest.errnoOf("fd_write", { 1, 300, 2, 328 }) == Errno::Fault);
    CHECK(guest.isUntouched(328, 4));
}

TEST_CASE("Preview1Shim.fd_write.totalBeyondU32IsInval")
{
    // 4100 buffers, each the whole 1 MiB memory: every one lies in the memory, but their total of
    // 4100 MiB is more than the u32 nwritten holds.
    constexpr auto Bytes = uint32_t { 1 } << 20U;
    constexpr auto Buffers = uint32_t { 4100 };
    auto memory = std::vector<std::byte>(Bytes, std::byte {});
    for (auto const index: std::views::iota(uint32_t { 0 }, Buffers))
        for (auto const offset: std::views::iota(uint32_t { 0 }, uint32_t { 4 }))
            memory.at((index * 8) + 4 + offset) = std::byte { static_cast<uint8_t>(Bytes >> (offset * 8U)) };
    auto const* const function = findShimFunction("fd_write");
    REQUIRE(function != nullptr);
    auto const environment = testEnvironment();
    auto const args = std::to_array<uint64_t>({ 1, 0, Buffers, Bytes - 4 });
    CHECK(function->call(environment, memory, args) == ShimResult { ShimOutcome::Returned, Errno::Inval });
}

TEST_CASE("Preview1Shim.fd_close")
{
    auto guest = Guest {};
    for (auto const fd: StandardStreams)
        CHECK(guest.errnoOf("fd_close", { fd }) == Errno::Success);
    // Closing a standard stream leaves the stream: writing to it still succeeds.
    guest.store32(300, 400);
    guest.store32(304, 1);
    CHECK(guest.errnoOf("fd_write", { 1, 300, 1, 320 }) == Errno::Success);
    CHECK(guest.errnoOf("fd_close", { 3 }) == Errno::Badf);
}

TEST_CASE("Preview1Shim.fd_fdstat_get")
{
    auto guest = Guest {};
    for (auto const fd: StandardStreams)
    {
        INFO("fd " << fd);
        CHECK(guest.errnoOf("fd_fdstat_get", { fd, 200 }) == Errno::Success);
        CHECK(guest.load(200, 1) == CharacterDevice); // fs_filetype
        CHECK(guest.load(202, 2) == 0);               // fs_flags
        CHECK(guest.load(208, 8) == StandardStreamRights);
        CHECK(guest.load(216, 8) == 0); // fs_rights_inheriting
    }
    CHECK(guest.isUntouched(224, 4));

    auto fresh = Guest {};
    CHECK(fresh.errnoOf("fd_fdstat_get", { 3, 200 }) == Errno::Badf);
    CHECK(fresh.isUntouched());
}

TEST_CASE("Preview1Shim.fd_prestat_get")
{
    auto guest = Guest {};
    // There is no preopened directory, so no descriptor has a prestat.
    for (auto const fd: { 0, 1, 2, 3, 4 })
        CHECK(guest.errnoOf("fd_prestat_get", { static_cast<uint64_t>(fd), 200 }) == Errno::Badf);
    CHECK(guest.isUntouched());
}

TEST_CASE("Preview1Shim.fd_prestat_dir_name")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("fd_prestat_dir_name", { 3, 200, 16 }) == Errno::Badf);
    CHECK(guest.isUntouched());
}

TEST_CASE("Preview1Shim.fd_seek")
{
    auto guest = Guest {};
    for (auto const fd: StandardStreams)
        for (auto const whence: { 0, 1, 2 })
        {
            INFO("fd " << fd << ", whence " << whence);
            CHECK(guest.errnoOf("fd_seek", { fd, 10, static_cast<uint64_t>(whence), 200 }) == Errno::Success);
            CHECK(guest.load(200, 8) == 0);
        }
    CHECK(guest.isUntouched(208, 4));

    auto fresh = Guest {};
    CHECK(fresh.errnoOf("fd_seek", { 1, 0, 3, 200 }) == Errno::Inval);
    CHECK(fresh.errnoOf("fd_seek", { 3, 0, 0, 200 }) == Errno::Badf);
    CHECK(fresh.isUntouched());
}

TEST_CASE("Preview1Shim.random_get")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("random_get", { 600, 16 }) == Errno::Success);
    for (auto const offset: std::views::iota(uint32_t { 0 }, uint32_t { 16 }))
        CHECK(guest.load(600 + offset, 1) == FirstRandomByte + offset);
    CHECK(guest.isUntouched(616, 4));
    CHECK(guest.isUntouched(596, 4));
    CHECK(guest.errnoOf("random_get", { 700, 0 }) == Errno::Success);
    CHECK(guest.isUntouched(700, 4));
}

TEST_CASE("Preview1Shim.proc_exit")
{
    auto guest = Guest {};
    CHECK(guest.call("proc_exit", { 0 }).outcome == ShimOutcome::Exit);
    CHECK(guest.call("proc_exit", { 3 }).outcome == ShimOutcome::Exit);
    CHECK(guest.isUntouched());
}

TEST_CASE("Preview1Shim.sched_yield")
{
    auto guest = Guest {};
    CHECK(guest.errnoOf("sched_yield", {}) == Errno::Success);
}

TEST_CASE("Preview1Shim.outOfBoundsPointerFaults")
{
    auto guest = Guest {};
    guest.store32(300, 400);
    guest.store32(304, 1);
    // Each pointer a function writes or reads, two bytes before the end; the other pointers in bounds.
    CHECK(guest.errnoOf("args_sizes_get", { NearEnd, 100 }) == Errno::Fault);
    CHECK(guest.errnoOf("args_sizes_get", { 100, NearEnd }) == Errno::Fault);
    CHECK(guest.errnoOf("environ_sizes_get", { NearEnd, 100 }) == Errno::Fault);
    CHECK(guest.errnoOf("environ_sizes_get", { 100, NearEnd }) == Errno::Fault);
    CHECK(guest.errnoOf("clock_time_get", { 0, 1, NearEnd }) == Errno::Fault);
    CHECK(guest.errnoOf("clock_res_get", { 0, NearEnd }) == Errno::Fault);
    CHECK(guest.errnoOf("fd_write", { 1, NearEnd, 1, 100 }) == Errno::Fault);
    CHECK(guest.errnoOf("fd_write", { 1, 300, 1, NearEnd }) == Errno::Fault);
    CHECK(guest.errnoOf("fd_fdstat_get", { 1, NearEnd }) == Errno::Fault);
    CHECK(guest.errnoOf("fd_seek", { 1, 0, 0, NearEnd }) == Errno::Fault);
    CHECK(guest.errnoOf("random_get", { NearEnd, 4 }) == Errno::Fault);
    // Functions that write nothing still check where they would: past the end, not even zero bytes fit.
    CHECK(guest.errnoOf("args_get", { MemoryBytes + 1, 100 }) == Errno::Fault);
    CHECK(guest.errnoOf("args_get", { 100, MemoryBytes + 1 }) == Errno::Fault);
    CHECK(guest.errnoOf("environ_get", { MemoryBytes + 1, 100 }) == Errno::Fault);
    CHECK(guest.errnoOf("environ_get", { 100, MemoryBytes + 1 }) == Errno::Fault);
    CHECK(guest.errnoOf("random_get", { MemoryBytes + 1, 0 }) == Errno::Fault);
    // An address that wraps around 32 bits.
    CHECK(guest.errnoOf("random_get", { std::numeric_limits<uint32_t>::max(), 2 }) == Errno::Fault);
    // fd_prestat_get and fd_prestat_dir_name find no descriptor first, and never reach their pointer.
    CHECK(guest.isUntouched(0, 300));
    CHECK(guest.isUntouched(308, MemoryBytes - 308));
}

TEST_CASE("Preview1Shim.systemEnvironment")
{
    auto const environment = systemEnvironment();
    auto const realtime = environment.clock(static_cast<uint32_t>(ClockId::Realtime));
    // 2020-01-01, in nanoseconds since the epoch: any real clock reads later.
    CHECK(realtime > uint64_t { 1'577'836'800 } * 1'000'000'000);
    auto const before = environment.clock(static_cast<uint32_t>(ClockId::Monotonic));
    auto const after = environment.clock(static_cast<uint32_t>(ClockId::Monotonic));
    CHECK(after >= before);

    // 64 random bytes are all zero with a chance of 2^-512.
    auto bytes = std::array<std::byte, 64> {};
    environment.random(bytes);
    CHECK(std::ranges::any_of(bytes, [](std::byte byte) { return byte != std::byte {}; }));
}
