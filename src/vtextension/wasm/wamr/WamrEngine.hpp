// SPDX-License-Identifier: Apache-2.0
//
// The WAMR engine an extension host process drives (host::IHostedEngine): the WAMR backend, interpreting its
// modules, set up for a host that locks itself down once its threads are prepared.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string_view>

#include <vtextension/wasm/host/IHostedEngine.hpp>
#include <vtextension/wasm/wamr/WamrBackend.hpp>

namespace vtextension::wasm::wamr
{

/// Whether a WAMR can compile guest code to native code.
enum class NativeCompilation : uint8_t
{
    Absent,  ///< It interprets only.
    Present, ///< It has a JIT running mode: Fast JIT, LLVM JIT, or both tiered.
};

/// @return Whether the WAMR this process links has a JIT running mode.
[[nodiscard]] NativeCompilation linkedNativeCompilation() noexcept;

/// Why no WAMR engine was made.
enum class WamrEngineError : uint8_t
{
    CompilesNativeCode,    ///< The WAMR can compile to native code, which the engine claims never to run.
    RuntimeUnavailable,    ///< WAMR could not initialize.
    NullDeviceUnavailable, ///< The null device could not be opened.
};

/// @param error Why no engine was made.
/// @return The reason, user-readable: it names the extension host's engine, never the runtime.
[[nodiscard]] std::string_view wamrEngineErrorText(WamrEngineError error) noexcept;

/// How a WAMR engine is made.
struct WamrEngineConfig
{
    /// Opens the null device, once.
    NullDeviceOpener openNullDevice = systemNullDeviceOpener();
    /// Says whether the linked WAMR compiles to native code.
    std::function<NativeCompilation()> nativeCompilation = &linkedNativeCompilation;
};

/// Creates the WAMR engine of an extension host process.
///
/// Everything it needs of the system it takes here, before the host locks itself down: one null device,
/// which every runtime it creates shares, and a hold on WAMR's process-wide runtime for as long as it lives,
/// so that its threads can be prepared before any runtime exists. Its instances' memory is copied to the
/// parent (MemoryMode::Copy), as WAMR places a linear memory where it chooses, and it needs no guard
/// (memoryGuardBytes() is 0). It interprets (GuestCode::Interpreted), and so refuses a WAMR that could
/// compile. prepareThread() enters WAMR's thread environment on the calling thread, which keeps it until it
/// ends.
/// @param config How it is made.
/// @return The engine; why none was made otherwise.
[[nodiscard]] std::expected<std::unique_ptr<host::IHostedEngine>, WamrEngineError> makeWamrEngine(
    WamrEngineConfig const& config = {});

} // namespace vtextension::wasm::wamr
