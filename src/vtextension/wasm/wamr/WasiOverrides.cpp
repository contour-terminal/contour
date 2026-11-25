// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wamr/WasiOverrides.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <wasm_export.h>

#include <vtextension/wasm/WasiAllowList.hpp>
#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>
#include <vtextension/wasm/wasi/Preview1Shim.hpp>
#include <vtextension/wasm/wasi/Preview1Signatures.hpp>
#include <vtextension/wasm/wasi/SystemEnvironment.hpp>

namespace vtextension::wasm::wamr
{

namespace
{
    /// WasiModule (../WasiAllowList.hpp), NUL-terminated as WAMR reads it.
    constexpr char const* WasiModuleName = "wasi_snapshot_preview1";
    static_assert(std::string_view { WasiModuleName } == WasiModule);

    /// @return Where the overrides read clocks and random bytes: the system's, as the shim's every caller.
    [[nodiscard]] wasi::ShimEnvironment const& environment()
    {
        static auto const system = wasi::systemEnvironment();
        return system;
    }

    /// @return The linear memory of the instance @p execEnv runs; empty if it has none.
    [[nodiscard]] std::span<std::byte> guestMemory(wasm_exec_env_t execEnv) noexcept
    {
        auto* const memory = wasm_runtime_get_default_memory(wasm_runtime_get_module_inst(execEnv));
        if (!memory)
            return {};
        auto const bytes = wasm_memory_get_cur_page_count(memory) * wasm_memory_get_bytes_per_page(memory);
        return { static_cast<std::byte*>(wasm_memory_get_base_address(memory)),
                 static_cast<std::size_t>(bytes) };
    }

    /// The raw native WasiOverrides[Index] is registered as: its arguments in WAMR's 64-bit slots, each
    /// zero-extended to its type as the shim takes them, and its errno written back into the first slot,
    /// where WAMR reads an i32 result.
    template <std::size_t Index>
    void shimTrampoline(wasm_exec_env_t execEnv, uint64_t* rawArguments) noexcept
    {
        static auto const* const function = wasi::findShimFunction(WasiOverrides.at(Index).name);
        auto const parameters = function->preview1.parameters();
        uint64_t* const first = rawArguments; // WAMR's slots, of WAMR's type.
        auto const slots = std::span { first, parameters.size() };
        auto arguments = std::array<uint64_t, wasi::MaxPreview1Parameters> {};
        std::ranges::transform(slots, parameters, arguments.begin(), [](uint64_t slot, ValueType type) {
            return type == ValueType::I32 ? (slot & 0xFFFF'FFFF) : slot;
        });
        auto const result = function->call(
            environment(), guestMemory(execEnv), std::span { arguments }.first(parameters.size()));
        auto const error = static_cast<uint32_t>(result.error);
        std::memcpy(first, &error, sizeof(error));
    }

    /// A raw native, of the very type WAMR calls it through.
    using RawNative = void (*)(wasm_exec_env_t, uint64_t*);

    template <std::size_t... Indices>
    [[nodiscard]] consteval auto makeTrampolines(std::index_sequence<Indices...> /*indices*/)
    {
        return std::array<RawNative, sizeof...(Indices)> { &shimTrampoline<Indices>... };
    }

    /// The trampolines, indexed as WasiOverrides.
    constexpr auto Trampolines = makeTrampolines(std::make_index_sequence<WasiOverrides.size()> {});

    /// What WAMR holds while registered, which it sorts in place: lives as long as the process.
    [[nodiscard]] std::array<NativeSymbol, WasiOverrides.size()>& symbols()
    {
        static auto table = [] {
            auto entries = std::array<NativeSymbol, WasiOverrides.size()> {};
            for (auto const index: std::views::iota(std::size_t { 0 }, WasiOverrides.size()))
                entries.at(index) = NativeSymbol { .symbol = WasiOverrides.at(index).name,
                                                   .func_ptr = reinterpret_cast<void*>(Trampolines.at(index)),
                                                   .signature = WasiOverrides.at(index).signature,
                                                   .attachment = nullptr };
            return entries;
        }();
        return table;
    }
} // namespace

namespace detail
{
    bool registerWasiOverrides()
    {
        static auto mutex = std::mutex {};
        static auto registeredFor = uint64_t { 0 };
        auto const lock = std::scoped_lock { mutex };
        if (registeredFor == runtimeGeneration())
            return true;
        auto& table = symbols();
        if (!wasm_runtime_register_natives_raw(
                WasiModuleName, table.data(), static_cast<uint32_t>(table.size())))
            return false;
        registeredFor = runtimeGeneration();
        return true;
    }
} // namespace detail

} // namespace vtextension::wasm::wamr
