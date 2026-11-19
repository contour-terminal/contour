// SPDX-License-Identifier: Apache-2.0
//
// The daemon world's exports, as bindings/daemon.h declares them: `load` starts the one Driver, `update`
// forwards each hint to it. WebAssembly only.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <bindings/daemon.h>
#include <orchestrator/core/Config.hpp>
#include <orchestrator/guest/Driver.hpp>
#include <orchestrator/guest/HostCalls.hpp>
#include <orchestrator/guest/WitEntropy.hpp>
#include <orchestrator/guest/WitHost.hpp>

namespace
{

using namespace orchestrator;
using namespace orchestrator::guest;

/// The manifest's root labels HostInfo is filled from.
constexpr auto WorktreesLabel = std::string_view { "worktrees" };
constexpr auto ReposLabel = std::string_view { "repos" };

[[nodiscard]] std::string_view viewOf(daemon_string_t const& text) noexcept
{
    return { reinterpret_cast<char const*>(text.ptr), text.len };
}

/// @return What the host resolved, as the engine takes it: the `worktrees` root's directory and the `repos`
///         root's directories, in the configuration's order.
[[nodiscard]] HostInfo hostInfoOf(daemon_host_info_t const& info)
{
    auto host = HostInfo { .home = std::string(viewOf(info.home)),
                           .worktreeRoot = {},
                           .repoPaths = {},
                           .utcOffsetMinutes = info.utc_offset_minutes,
                           .selfPrograms = {} };
    for (auto const& program: std::span(info.self_programs.ptr, info.self_programs.len))
        host.selfPrograms.push_back(HostProgram { .name = std::string(viewOf(program.name)),
                                                  .path = std::string(viewOf(program.path)) });
    for (auto const& root: std::span(info.roots.ptr, info.roots.len))
    {
        auto const paths = std::span(root.paths.ptr, root.paths.len);
        if (viewOf(root.label) == WorktreesLabel && !paths.empty())
            host.worktreeRoot = std::string(viewOf(paths.front()));
        else if (viewOf(root.label) == ReposLabel)
            for (auto const& path: paths)
                host.repoPaths.emplace_back(viewOf(path));
    }
    return host;
}

/// @return The driver; empty until `load`.
[[nodiscard]] std::optional<Driver>& driver()
{
    static auto instance = std::optional<Driver> {};
    return instance;
}

} // namespace

extern "C"
{
    // bindings/daemon.c names the symbol of the component-type object `wit-bindgen c --no-object-file` leaves
    // out; a core module carries no component type, so it is defined empty.
    void __component_type_object_force_link_daemon(void)
    {
    }

    void exports_daemon_load(daemon_string_t* configJson, daemon_host_info_t* info)
    {
        auto config = std::string(viewOf(*configJson));
        auto host = hostInfoOf(*info);
        // The arguments were allocated for the export through cabi_realloc, and belong to it.
        daemon_string_free(configJson);
        daemon_host_info_free(info);
        driver().emplace(Driver::load(witHost(), witEntropy(), config, std::move(host)));
    }

    void exports_daemon_update(daemon_hint_t hint)
    {
        if (!driver() || hint > TERMINAL_EXTENSIONS_DAEMON_TYPES_HINT_ACTIONS)
            return;
        driver()->update(static_cast<Hint>(hint));
    }
}
