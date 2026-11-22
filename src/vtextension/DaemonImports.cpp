// SPDX-License-Identifier: Apache-2.0
#include <vtextension/DaemonImports.hpp>

#include <algorithm>
#include <array>
#include <span>

namespace vtextension
{

namespace
{
    /// DaemonImportRows as the backend registers them. Each name is a string literal, so it is
    /// NUL-terminated where a backend needs it to be.
    constexpr auto DaemonHostFunctions = [] {
        auto functions = std::array<wasm::HostFunction, DaemonImportRows.size()> {};
        std::ranges::transform(DaemonImportRows, functions.begin(), [](DaemonImportRow const& row) {
            return wasm::HostFunction { .module = DaemonImportModule,
                                        .name = row.name,
                                        .parameterCount = row.parameterCount };
        });
        return functions;
    }();
} // namespace

std::span<wasm::HostFunction const> daemonHostFunctions() noexcept
{
    return DaemonHostFunctions;
}

} // namespace vtextension
