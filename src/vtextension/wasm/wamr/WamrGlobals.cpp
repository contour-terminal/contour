// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wamr/WamrGlobals.hpp>

#include <cstdint>

namespace vtextension::wasm::wamr::detail
{

namespace
{
    /// The lock every caller in this process takes over WAMR's globals.
    [[nodiscard]] std::shared_mutex& wamrGlobalsMutex()
    {
        static auto mutex = std::shared_mutex {};
        return mutex;
    }
} // namespace

std::expected<void, ExtensionError> registerNativesRaw(char const* module, std::span<NativeSymbol> symbols)
{
    auto const lock = changingWamrGlobals();
    if (!wasm_runtime_register_natives_raw(module, symbols.data(), static_cast<uint32_t>(symbols.size())))
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    return {};
}

void unregisterNatives(char const* module, NativeSymbol* symbols) noexcept
{
    auto const lock = changingWamrGlobals();
    wasm_runtime_unregister_natives(module, symbols);
}

std::shared_lock<std::shared_mutex> readingWamrGlobals()
{
    return std::shared_lock { wamrGlobalsMutex() };
}

std::unique_lock<std::shared_mutex> changingWamrGlobals()
{
    return std::unique_lock { wamrGlobalsMutex() };
}

} // namespace vtextension::wasm::wamr::detail
