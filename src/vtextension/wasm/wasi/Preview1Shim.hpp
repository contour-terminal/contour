// SPDX-License-Identifier: Apache-2.0
//
// The host's own implementation of every allowed WASI preview 1 function, written against guest memory
// and an injected environment alone, so that every engine binding it behaves the same: no arguments,
// no environment variables, no preopened directory, and three standard streams that write nowhere.
// Engine-independent: an engine binds each row under wasm::WasiModule and does nothing but translate.
#pragma once

#include <cstddef>
#include <cstdint>
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

/// What the shim reads from outside guest memory: injected, so that tests are deterministic.
/// Neither function may throw: an engine calls the shim from code an exception must not unwind through.
struct ShimEnvironment
{
    /// @return The time of the clock @p clockId (ClockId::Realtime or ClockId::Monotonic only), in
    ///         nanoseconds.
    std::function<uint64_t(uint32_t clockId)> clock;
    /// Fills its argument with random bytes.
    std::function<void(std::span<std::byte>)> random;
};

/// A WASI preview 1 errno, by its value; only those the shim returns (a u16 on the wire; every value of
/// preview 1 fits a byte).
enum class Errno : uint8_t
{
    Success = 0, ///< The function succeeded.
    Badf = 8,    ///< No such file descriptor.
    Fault = 21,  ///< A pointer the guest passed lies outside its memory.
    Inval = 28,  ///< An argument is out of range.
    Nosys = 52,  ///< The function, or the clock asked for, is not provided.
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
