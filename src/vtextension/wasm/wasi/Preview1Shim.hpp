// SPDX-License-Identifier: Apache-2.0
//
// The host's own implementation of every allowed WASI preview 1 function, written against guest memory
// and an injected environment alone, so that every engine binding it behaves the same: no arguments,
// no environment variables, no preopened directory, and three standard streams that write nowhere.
// Engine-independent: an engine binds each row under wasm::WasiModule and does nothing but translate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string_view>

#include <vtextension/wasm/wasi/Preview1Signatures.hpp>

namespace vtextension::wasm::wasi
{

/// A WASI preview 1 clock, by its `clockid` value (a u32 on the wire; every value fits a byte).
enum class ClockId : uint8_t
{
    Realtime = 0,       ///< Wall-clock time, since the Unix epoch.
    Monotonic = 1,      ///< A clock that never goes back, from an unspecified start.
    ProcessCputime = 2, ///< The CPU time of the process; not provided (Errno::Nosys).
    ThreadCputime = 3,  ///< The CPU time of the thread; not provided (Errno::Nosys).
};

/// A WASI preview 1 errno, by its value; only those the shim returns (a u16 on the wire; every value of
/// preview 1 fits a byte).
enum class Errno : uint8_t
{
    Success = 0, ///< The function succeeded.
    Badf = 8,    ///< No such file descriptor.
    Fault = 21,  ///< A pointer the guest passed lies outside its memory.
    Inval = 28,  ///< An argument is out of range.
    Io = 29,     ///< The host could not do what was asked of it (its random source failed).
    Nosys = 52,  ///< The function, or the clock asked for, is not provided.
};

/// What the shim reads from outside guest memory: injected, so that tests are deterministic.
/// Neither function may throw: an engine calls the shim from code an exception must not unwind through.
class ShimEnvironment
{
  public:
    /// Reads a clock, ClockId::Realtime or ClockId::Monotonic only (the shim asks for no other).
    /// @return The clock's time, in nanoseconds.
    using Clock = std::function<uint64_t(ClockId clockId)>;

    /// Fills its argument with random bytes.
    /// @return Nothing once filled; `Io` if the source refused, which fails only the calling guest's
    ///         random_get, never the process.
    using Random = std::function<std::expected<void, Errno>(std::span<std::byte>)>;

    /// Both functions are required: an empty one would end the process the first time a guest reached
    /// it, deep inside a guest call, so it ends it here instead (a precondition, as passing one is a
    /// programmer error).
    /// @param clock Where clock_time_get reads its clocks.
    /// @param random Where random_get takes its bytes.
    ShimEnvironment(Clock clock, Random random);

    /// @return The time of @p clockId, ClockId::Realtime or ClockId::Monotonic, in nanoseconds.
    [[nodiscard]] uint64_t clock(ClockId clockId) const { return _clock(clockId); }

    /// Fills @p bytes with random bytes.
    /// @return Nothing once filled; `Io` if the source refused.
    [[nodiscard]] std::expected<void, Errno> random(std::span<std::byte> bytes) const
    {
        return _random(bytes);
    }

  private:
    Clock _clock;
    Random _random;
};

/// How a shim function ended.
enum class ShimOutcome : uint8_t
{
    Returned, ///< It returns its errno to the guest.
    Exit,     ///< proc_exit: it does not return, and the engine ends the call as a trap.
};

/// What a shim function ended with.
struct ShimResult
{
    ShimOutcome outcome = ShimOutcome::Returned; ///< How it ended.
    Errno error = Errno::Success;                ///< Its errno, when it returned.

    bool operator==(ShimResult const&) const = default;
};

/// Runs one shim function.
/// @param environment Where clocks and random bytes come from.
/// @param memory The guest's linear memory; every guest pointer is checked against it.
/// @param args The guest's arguments, in order, each zero-extended to 64 bits (i32 and i64 alike).
/// @return How the function ended.
using ShimCall = ShimResult (*)(ShimEnvironment const& environment,
                                std::span<std::byte> memory,
                                std::span<uint64_t const> args);

/// One allowed WASI preview 1 function and its implementation.
struct ShimFunction
{
    Preview1Function preview1; ///< Its name and signature, the row of Preview1Functions.
    ShimCall call = nullptr;   ///< What a call to it runs.
};

/// @return One row per entry of AllowedWasiFunctions, in its order.
[[nodiscard]] std::span<ShimFunction const> shimFunctions() noexcept;

/// @return The shim function named @p name; nullptr if it is not an allowed WASI function.
[[nodiscard]] ShimFunction const* findShimFunction(std::string_view name) noexcept;

} // namespace vtextension::wasm::wasi
