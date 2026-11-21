// SPDX-License-Identifier: Apache-2.0
#include <vtextension/HostBindings.hpp>

#include <algorithm>
#include <span>
#include <vector>

#include <vtextension/DaemonImports.hpp>

namespace vtextension::detail
{

namespace
{
    /// @return The (module, name) pairs of @p functions.
    [[nodiscard]] std::vector<WorldImport> importsOf(std::span<wasm::HostFunction const> functions)
    {
        auto imports = std::vector<WorldImport>(functions.size());
        std::ranges::transform(functions, imports.begin(), [](wasm::HostFunction const& function) {
            return WorldImport { .module = function.module, .name = function.name };
        });
        return imports;
    }
} // namespace

std::span<wasm::HostFunction const> hostFunctions(World world) noexcept
{
    switch (world)
    {
        case World::StatusLine: return statuslineHostFunctions();
        case World::Daemon: return daemonHostFunctions();
    }
    return {};
}

std::span<WorldImport const> worldImports(World world) noexcept
{
    // Derived from the host function tables, so the list a module is checked against and what its runtime
    // registers cannot disagree.
    static auto const statusLine = importsOf(hostFunctions(World::StatusLine));
    static auto const daemon = importsOf(hostFunctions(World::Daemon));
    switch (world)
    {
        case World::StatusLine: return statusLine;
        case World::Daemon: return daemon;
    }
    return {};
}

} // namespace vtextension::detail
