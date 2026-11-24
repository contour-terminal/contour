// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wasi/Preview1Shim.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>

#include <vtextension/wasm/WasiAllowList.hpp>

namespace vtextension::wasm::wasi
{

namespace
{
    /// The standard streams are file descriptors 0 to 2; there is no other.
    constexpr auto StandardStreamCount = uint32_t { 3 };

    /// The size of a WASI `ciovec`: a u32 buffer address, then a u32 length.
    constexpr auto CiovecBytes = uint64_t { 8 };

    /// The size of a WASI `fdstat`, and where its fields lie.
    constexpr auto FdstatBytes = uint32_t { 24 };
    constexpr auto FdstatRightsBaseOffset = std::size_t { 8 };

    /// The `filetype` of a character device, which every standard stream is.
    constexpr auto CharacterDevice = std::byte { 2 };

    /// The `rights` of a standard stream: fd_seek (bit 2) and fd_write (bit 6), the operations the
    /// shim implements on it.
    constexpr auto StandardStreamRights = uint64_t { 0b0100'0100 };

    /// The highest `whence` of fd_seek: set (0), cur (1) and end (2).
    constexpr auto HighestWhence = uint64_t { 2 };

    /// The resolution every provided clock reports, in nanoseconds.
    constexpr auto ClockResolution = uint64_t { 1 };

    /// Whether the shim provides a clock of preview 1.
    enum class ClockProvision : uint8_t
    {
        Withheld, ///< Not provided: reading it gives Errno::Nosys.
        Provided, ///< Read from ShimEnvironment::clock.
    };

    /// One clock of preview 1, and whether it is provided.
    struct ClockRow
    {
        ClockId id;               ///< The clock.
        ClockProvision provision; ///< Whether it is provided.
    };

    /// Every clock of preview 1. The CPU-time clocks are withheld: an instance's CPU time is the host's
    /// business, and preview 1 lets a host answer `nosys` for a clock it does not support.
    constexpr auto Clocks = std::to_array<ClockRow>({
        { .id = ClockId::Realtime, .provision = ClockProvision::Provided },
        { .id = ClockId::Monotonic, .provision = ClockProvision::Provided },
        { .id = ClockId::ProcessCputime, .provision = ClockProvision::Withheld },
        { .id = ClockId::ThreadCputime, .provision = ClockProvision::Withheld },
    });

    /// @return The i32 argument @p arg, as the guest passed it.
    [[nodiscard]] constexpr uint32_t asU32(uint64_t arg) noexcept
    {
        return static_cast<uint32_t>(arg);
    }

    /// @return The @p size bytes of @p memory at @p address; `Fault` if they do not all lie in it.
    [[nodiscard]] std::expected<std::span<std::byte>, Errno> range(std::span<std::byte> memory,
                                                                   uint32_t address,
                                                                   uint64_t size) noexcept
    {
        if (uint64_t { address } + size > memory.size())
            return std::unexpected(Errno::Fault);
        return memory.subspan(address, static_cast<std::size_t>(size));
    }

    /// Stores @p value into @p bytes, little-endian, in as many bytes as @p bytes has.
    template <std::unsigned_integral T>
    void storeLittleEndian(std::span<std::byte> bytes, T value) noexcept
    {
        for (auto& byte: bytes)
        {
            byte = std::byte { static_cast<uint8_t>(value) };
            value = static_cast<T>(value >> 8U);
        }
    }

    /// @return The little-endian u32 at the start of @p bytes, which holds at least four.
    [[nodiscard]] uint32_t loadU32(std::span<std::byte const> bytes) noexcept
    {
        auto value = uint32_t { 0 };
        for (auto const byte: bytes.first(4) | std::views::reverse)
            value = (value << 8U) | std::to_integer<uint32_t>(byte);
        return value;
    }

    /// Stores @p value as a little-endian u32 at @p address.
    /// @return Nothing; `Fault` if the four bytes do not lie in @p memory.
    [[nodiscard]] std::expected<void, Errno> storeU32(std::span<std::byte> memory,
                                                      uint32_t address,
                                                      uint32_t value) noexcept
    {
        return range(memory, address, sizeof(value)).transform([&](std::span<std::byte> bytes) {
            storeLittleEndian(bytes, value);
        });
    }

    /// Stores @p value as a little-endian u64 at @p address.
    /// @return Nothing; `Fault` if the eight bytes do not lie in @p memory.
    [[nodiscard]] std::expected<void, Errno> storeU64(std::span<std::byte> memory,
                                                      uint32_t address,
                                                      uint64_t value) noexcept
    {
        return range(memory, address, sizeof(value)).transform([&](std::span<std::byte> bytes) {
            storeLittleEndian(bytes, value);
        });
    }

    /// @return The result of a function that returned @p outcome's errno.
    [[nodiscard]] ShimResult returned(std::expected<void, Errno> const& outcome) noexcept
    {
        return ShimResult { .outcome = ShimOutcome::Returned,
                            .error = outcome ? Errno::Success : outcome.error() };
    }

    /// @return The standard stream @p fd names; `Badf` if it names none.
    [[nodiscard]] std::expected<uint32_t, Errno> standardStream(uint64_t fd) noexcept
    {
        if (asU32(fd) >= StandardStreamCount)
            return std::unexpected(Errno::Badf);
        return asU32(fd);
    }

    /// @return The provided clock @p clockId names; `Nosys` if it is withheld, `Inval` if it names none.
    [[nodiscard]] std::expected<ClockId, Errno> providedClock(uint64_t clockId) noexcept
    {
        auto const row = std::ranges::find(
            Clocks, asU32(clockId), [](ClockRow const& clock) { return static_cast<uint32_t>(clock.id); });
        if (row == Clocks.end())
            return std::unexpected(Errno::Inval);
        if (row->provision == ClockProvision::Withheld)
            return std::unexpected(Errno::Nosys);
        return row->id;
    }

    /// args_get and environ_get, of an empty list: (list, buffer) -> errno. Writes nothing.
    ShimResult emptyListGet(ShimEnvironment const& /*environment*/,
                            std::span<std::byte> memory,
                            std::span<uint64_t const> args)
    {
        return returned(range(memory, asU32(args[0]), 0)
                            .and_then([&](auto) { return range(memory, asU32(args[1]), 0); })
                            .transform([](auto) {}));
    }

    /// args_sizes_get and environ_sizes_get, of an empty list: (count, size) -> errno. Stores 0 into both,
    /// or into neither.
    ShimResult emptyListSizesGet(ShimEnvironment const& /*environment*/,
                                 std::span<std::byte> memory,
                                 std::span<uint64_t const> args)
    {
        return returned(range(memory, asU32(args[0]), sizeof(uint32_t)).and_then([&](auto count) {
            return range(memory, asU32(args[1]), sizeof(uint32_t)).transform([&](auto size) {
                storeLittleEndian(count, uint32_t { 0 });
                storeLittleEndian(size, uint32_t { 0 });
            });
        }));
    }

    /// clock_time_get: (clock, precision, time) -> errno. The precision is a hint, and ignored.
    ShimResult clockTimeGet(ShimEnvironment const& environment,
                            std::span<std::byte> memory,
                            std::span<uint64_t const> args)
    {
        return returned(providedClock(args[0]).and_then([&](ClockId clock) {
            return range(memory, asU32(args[2]), sizeof(uint64_t)).transform([&](auto bytes) {
                storeLittleEndian(bytes, environment.clock(static_cast<uint32_t>(clock)));
            });
        }));
    }

    /// clock_res_get: (clock, resolution) -> errno.
    ShimResult clockResGet(ShimEnvironment const& /*environment*/,
                           std::span<std::byte> memory,
                           std::span<uint64_t const> args)
    {
        return returned(providedClock(args[0]).and_then(
            [&](ClockId) { return storeU64(memory, asU32(args[1]), ClockResolution); }));
    }

    /// @return The total length of the `ciovec`s @p iovs, each of whose buffers lies in @p memory;
    ///         `Fault` if one does not, `Inval` if the total does not fit a u32.
    [[nodiscard]] std::expected<uint32_t, Errno> totalLength(std::span<std::byte> memory,
                                                             std::span<std::byte const> iovs) noexcept
    {
        auto total = uint64_t { 0 };
        for (auto const index: std::views::iota(uint64_t { 0 }, iovs.size() / CiovecBytes))
        {
            auto const iov = iovs.subspan(static_cast<std::size_t>(index * CiovecBytes), CiovecBytes);
            auto const length = loadU32(iov.subspan(4));
            if (!range(memory, loadU32(iov), length))
                return std::unexpected(Errno::Fault);
            total += length;
        }
        if (total > std::numeric_limits<uint32_t>::max())
            return std::unexpected(Errno::Inval);
        return static_cast<uint32_t>(total);
    }

    /// fd_write: (fd, iovs, iovsLength, written) -> errno. Writes nowhere, and reports every byte written.
    ShimResult fdWrite(ShimEnvironment const& /*environment*/,
                       std::span<std::byte> memory,
                       std::span<uint64_t const> args)
    {
        return returned(
            standardStream(args[0])
                .and_then(
                    [&](uint32_t) { return range(memory, asU32(args[1]), asU32(args[2]) * CiovecBytes); })
                .and_then([&](std::span<std::byte> iovs) { return totalLength(memory, iovs); })
                .and_then([&](uint32_t total) { return storeU32(memory, asU32(args[3]), total); }));
    }

    /// fd_close: (fd) -> errno. A standard stream stays open: it never held anything to release.
    ShimResult fdClose(ShimEnvironment const& /*environment*/,
                       std::span<std::byte> /*memory*/,
                       std::span<uint64_t const> args)
    {
        return returned(standardStream(args[0]).transform([](uint32_t) {}));
    }

    /// fd_fdstat_get: (fd, stat) -> errno. A standard stream is a character device with no flags.
    ShimResult fdFdstatGet(ShimEnvironment const& /*environment*/,
                           std::span<std::byte> memory,
                           std::span<uint64_t const> args)
    {
        return returned(standardStream(args[0])
                            .and_then([&](uint32_t) { return range(memory, asU32(args[1]), FdstatBytes); })
                            .transform([](std::span<std::byte> stat) {
                                std::ranges::fill(stat, std::byte {});
                                stat.front() = CharacterDevice;
                                storeLittleEndian(stat.subspan(FdstatRightsBaseOffset, sizeof(uint64_t)),
                                                  StandardStreamRights);
                            }));
    }

    /// fd_prestat_get and fd_prestat_dir_name: there is no preopened directory, so no descriptor has one.
    ShimResult noPreopenedDirectory(ShimEnvironment const& /*environment*/,
                                    std::span<std::byte> /*memory*/,
                                    std::span<uint64_t const> /*args*/)
    {
        return returned(std::unexpected(Errno::Badf));
    }

    /// fd_seek: (fd, offset, whence, newOffset) -> errno. A standard stream stays at offset 0.
    ShimResult fdSeek(ShimEnvironment const& /*environment*/,
                      std::span<std::byte> memory,
                      std::span<uint64_t const> args)
    {
        return returned(standardStream(args[0]).and_then([&](uint32_t) -> std::expected<void, Errno> {
            if (asU32(args[2]) > HighestWhence)
                return std::unexpected(Errno::Inval);
            return storeU64(memory, asU32(args[3]), 0);
        }));
    }

    /// random_get: (buffer, length) -> errno.
    ShimResult randomGet(ShimEnvironment const& environment,
                         std::span<std::byte> memory,
                         std::span<uint64_t const> args)
    {
        return returned(
            range(memory, asU32(args[0]), asU32(args[1])).transform([&](std::span<std::byte> bytes) {
                environment.random(bytes);
            }));
    }

    /// proc_exit: (code). Does not return; the exit code is not kept.
    ShimResult procExit(ShimEnvironment const& /*environment*/,
                        std::span<std::byte> /*memory*/,
                        std::span<uint64_t const> /*args*/)
    {
        return ShimResult { .outcome = ShimOutcome::Exit, .error = Errno::Success };
    }

    /// sched_yield: () -> errno. An instance is single-threaded: there is nothing to yield to.
    ShimResult schedYield(ShimEnvironment const& /*environment*/,
                          std::span<std::byte> /*memory*/,
                          std::span<uint64_t const> /*args*/)
    {
        return returned({});
    }

    /// @return The row implementing @p name by @p call, with its signature from Preview1Functions.
    [[nodiscard]] consteval ShimFunction shim(std::string_view name, ShimCall call)
    {
        return ShimFunction { .preview1 = *findPreview1Function(name), .call = call };
    }

    constexpr auto ShimTable = std::to_array<ShimFunction>({
        shim("args_get", &emptyListGet),
        shim("args_sizes_get", &emptyListSizesGet),
        shim("environ_get", &emptyListGet),
        shim("environ_sizes_get", &emptyListSizesGet),
        shim("clock_time_get", &clockTimeGet),
        shim("clock_res_get", &clockResGet),
        shim("fd_write", &fdWrite),
        shim("fd_close", &fdClose),
        shim("fd_fdstat_get", &fdFdstatGet),
        shim("fd_prestat_get", &noPreopenedDirectory),
        shim("fd_prestat_dir_name", &noPreopenedDirectory),
        shim("fd_seek", &fdSeek),
        shim("random_get", &randomGet),
        shim("proc_exit", &procExit),
        shim("sched_yield", &schedYield),
    });

    static_assert(std::ranges::equal(ShimTable | std::views::transform([](ShimFunction const& function) {
                                         return function.preview1.name;
                                     }),
                                     AllowedWasiFunctions),
                  "ShimTable implements AllowedWasiFunctions, in its order");
} // namespace

std::span<ShimFunction const> shimFunctions() noexcept
{
    return ShimTable;
}

ShimFunction const* findShimFunction(std::string_view name) noexcept
{
    auto const row = std::ranges::find_if(
        ShimTable, [name](ShimFunction const& function) { return function.preview1.name == name; });
    return row != ShimTable.end() ? &*row : nullptr;
}

} // namespace vtextension::wasm::wasi
