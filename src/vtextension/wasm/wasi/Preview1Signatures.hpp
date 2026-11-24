// SPDX-License-Identifier: Apache-2.0
//
// The core signature of every WASI preview 1 function the extension host allows, as a guest imports it:
// what a backend checks an import against before it calls the import satisfied, and what it provides.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <vtextension/wasm/WasiAllowList.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm::wasi
{

/// What a WASI preview 1 function returns.
enum class Preview1Result : uint8_t
{
    Nothing, ///< No result: the function does not return to the guest (proc_exit).
    Errno,   ///< One i32, the function's errno.
};

/// The most parameters an allowed WASI preview 1 function takes.
inline constexpr auto MaxPreview1Parameters = std::size_t { 4 };

/// One WASI preview 1 function and its core signature.
struct Preview1Function
{
    std::string_view name;                                          ///< Its name.
    std::array<ValueType, MaxPreview1Parameters> parameterSlots {}; ///< Its parameters, then unused.
    std::size_t parameterCount = 0;                                 ///< How many parameters it takes.
    Preview1Result result = Preview1Result::Nothing;                ///< What it returns.

    /// @return Its parameter types, in order.
    [[nodiscard]] constexpr std::span<ValueType const> parameters() const noexcept
    {
        return std::span { parameterSlots }.first(parameterCount);
    }

    /// @return Its core signature, as a module importing it declares it.
    [[nodiscard]] FunctionSignature signature() const
    {
        auto const types = parameters();
        return FunctionSignature {
            .parameters = { types.begin(), types.end() },
            .results =
                result == Preview1Result::Errno ? std::vector { ValueType::I32 } : std::vector<ValueType> {},
        };
    }
};

namespace detail
{
    /// @return The row of @p name, taking @p parameters and returning @p result.
    [[nodiscard]] consteval Preview1Function preview1(std::string_view name,
                                                      std::initializer_list<ValueType> parameters,
                                                      Preview1Result result)
    {
        auto function =
            Preview1Function { .name = name, .parameterCount = parameters.size(), .result = result };
        std::ranges::copy(parameters, function.parameterSlots.begin());
        return function;
    }

    constexpr auto I32 = ValueType::I32;
    constexpr auto I64 = ValueType::I64;
    constexpr auto Errno = Preview1Result::Errno;
} // namespace detail

/// The signature of every function in AllowedWasiFunctions, in its order (the WASI preview 1 witx
/// definitions, lowered to core types: pointers, sizes, file descriptors and enums are i32, timestamps,
/// file offsets and sizes of 64 bits are i64).
inline constexpr auto Preview1Functions = std::to_array<Preview1Function>({
    detail::preview1("args_get", { detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("args_sizes_get", { detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("environ_get", { detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("environ_sizes_get", { detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("clock_time_get", { detail::I32, detail::I64, detail::I32 }, detail::Errno),
    detail::preview1("clock_res_get", { detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("fd_write", { detail::I32, detail::I32, detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("fd_close", { detail::I32 }, detail::Errno),
    detail::preview1("fd_fdstat_get", { detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("fd_prestat_get", { detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("fd_prestat_dir_name", { detail::I32, detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("fd_seek", { detail::I32, detail::I64, detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("random_get", { detail::I32, detail::I32 }, detail::Errno),
    detail::preview1("proc_exit", { detail::I32 }, Preview1Result::Nothing),
    detail::preview1("sched_yield", {}, detail::Errno),
});

static_assert(std::ranges::equal(Preview1Functions | std::views::transform(&Preview1Function::name),
                                 AllowedWasiFunctions),
              "Preview1Functions names AllowedWasiFunctions, in its order");

/// @return The allowed WASI preview 1 function named @p name; nullptr if it is not allowed.
[[nodiscard]] constexpr Preview1Function const* findPreview1Function(std::string_view name) noexcept
{
    auto const row = std::ranges::find(Preview1Functions, name, &Preview1Function::name);
    return row != Preview1Functions.end() ? &*row : nullptr;
}

} // namespace vtextension::wasm::wasi
