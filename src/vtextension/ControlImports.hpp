// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The control interface's host functions: `terminal-extensions:control/control@0.1.0`, one import per verb
/// of the control API that an extension may call (`vtcontrol::Exposure::Everywhere`), in catalog order.
///
/// Free of glaze, like every vtextension header but ControlCodec.hpp: what lifts and lowers the vocabulary
/// stays in ControlImports.cpp.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>

#include <vtcontrol/Catalog.hpp>
#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// The import module the control verbs live under: `<package>/<interface>@<version>`.
inline constexpr auto ControlImportModule = std::string_view { "terminal-extensions:control/control@0.1.0" };

/// How many host functions the control interface has: one per catalog row an extension may call.
inline constexpr std::size_t ControlImportCount = std::apply(
    [](auto const&... row) {
        return (std::size_t { 0 } + ... + (row.exposure == vtcontrol::Exposure::Everywhere ? 1U : 0U));
    },
    vtcontrol::Methods);

/// The Canonical ABI's limit on flat parameters: a request that flattens to more is passed by address.
inline constexpr std::size_t MaxFlatParameters = 16;

/// The control interface's host functions, in catalog order: each named like its WIT func (`pane-split`),
/// taking its request's flat values and then the address its `result` is stored at -- or, for a request of
/// more than MaxFlatParameters flat values, the request's address and the result's.
/// @return The table; it lives as long as the process.
[[nodiscard]] std::span<wasm::HostFunction const> controlHostFunctions() noexcept;

/// What a control import may spend: the rest of the guest call's deadline, which keeps running while the
/// host serves the verb.
struct ControlCallBudget
{
    /// How much of the guest call's deadline is left; what the host may wait for its answer.
    std::chrono::milliseconds remaining;
    /// The deadline's state, which the watchdog sets to Expired, from its own thread, when it passes.
    std::atomic<DeadlineState> const& deadline;
};

/// Serves one control import, as the guest called it.
///
/// Lifts the request (a guest that breaks the Canonical ABI traps), then answers, without calling the host,
/// `forbidden` for a verb whose permission the manifest does not declare or the user has not granted
/// ("not granted yet", "denied"), and `bad-request` for a `pane.send` that asks to wait: a guest must not
/// block on a shell. That refusal is this function's own rule; `vtcontrol::dispatch` knows nothing of it.
/// Otherwise the request goes to IHostServices::callControl as JSON, with the budget's remaining time, and
/// its reply is decoded as the verb's reply.
///
/// **No late reply is written.** If the deadline has expired by the time the host answers, the call fails
/// with `DeadlineExceeded` and nothing is stored: the guest is being interrupted, and an answer the host
/// gave after the guest's time was up must not reach its memory.
/// @param importIndex The function's index in controlHostFunctions().
/// @param memory The guest's memory.
/// @param arguments The function's flat arguments, the result's address last.
/// @param services Who serves the verb, and who knows whether the user granted it.
/// @param declared The control permissions the extension's manifest declares.
/// @param budget What the call may spend.
/// @return Nothing once the result is stored; `Trapped` for a guest that broke the ABI, `DeadlineExceeded`
///         for a call whose deadline passed, or what storing the result failed with.
[[nodiscard]] abi::Result<void> callControlImport(std::size_t importIndex,
                                                  abi::IGuestMemory& memory,
                                                  std::span<uint32_t const> arguments,
                                                  IHostServices& services,
                                                  vtcontrol::ControlGrants declared,
                                                  ControlCallBudget budget);

/// @param permissions The control permissions a manifest declares.
/// @return The same, as a set.
[[nodiscard]] vtcontrol::ControlGrants controlGrantsOf(
    std::span<vtcontrol::Permission const> permissions) noexcept;

} // namespace vtextension
