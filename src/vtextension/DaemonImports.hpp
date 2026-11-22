// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon world's imports: `terminal-extensions:daemon/host@0.1.0`, one row per func of the WIT's
/// `host` interface (wit/daemon/world.wit), in the WIT's order. The table is the exact list of imports a
/// daemon-world module may link against; adding an import is adding a row.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// The import module the daemon world's host functions live under: `<package>/<interface>@<version>`.
inline constexpr auto DaemonImportModule = std::string_view { "terminal-extensions:daemon/host@0.1.0" };

/// The daemon packages this host serves; a module importing another version's module is refused.
inline constexpr auto ServedDaemonPackageVersions = std::array { std::string_view { "0.1.0" } };

/// One import of the daemon world, named after its WIT func; the value indexes DaemonImportRows.
enum class DaemonImport : uint8_t
{
    Log,         ///< `log`
    Wake,        ///< `wake`
    Now,         ///< `now`
    ProcStart,   ///< `proc-start`
    Procs,       ///< `procs`
    ProcTake,    ///< `proc-take`
    ProcRelease, ///< `proc-release`
    StoreGet,    ///< `store-get`
    StoreSet,    ///< `store-set`
    StoreRemove, ///< `store-remove`
    StoreKeys,   ///< `store-keys`
    PaneOpen,    ///< `pane-open`
    PaneClose,   ///< `pane-close`
    Panes,       ///< `panes`
    TabOpen,     ///< `tab-open`
    TabClose,    ///< `tab-close`
    TabReveal,   ///< `tab-reveal`
    Tabs,        ///< `tabs`
    TabRead,     ///< `tab-read`
    TabWrite,    ///< `tab-write`
    ActionsTake, ///< `actions-take`
    SummarySet,  ///< `summary-set`
    PaneReveal,  ///< `pane-reveal`
    FileRead,    ///< `file-read`
    PaneSend,    ///< `pane-send`
};

/// Whether this host serves an import yet. Every import is declared and linkable from the first version
/// on; one not served yet answers `not-supported-yet` (or `file-error::not-supported-yet`), which a guest
/// treats as a capability it does not have, never as a fault. A later phase flips a row.
enum class ImportAvailability : uint8_t
{
    NotSupportedYet, ///< The import answers `not-supported-yet`.
    Served,          ///< The host serves the import.
};

/// One import of the daemon world: what the host registers it as, and the WIT it is generated from.
struct DaemonImportRow
{
    DaemonImport import = DaemonImport::Log; ///< The import this row describes; its value is the row's index.
    std::string_view name;                   ///< The WIT func's name, which is the import's field name.
    uint32_t parameterCount = 0;   ///< The Canonical ABI's flat parameters, the result's address included.
    std::string_view witSignature; ///< The func's line in the WIT, without the trailing `;`.
    ImportAvailability availability = ImportAvailability::NotSupportedYet; ///< Whether this host serves it.
};

/// The daemon world's imports, indexed by DaemonImport.
///
/// `parameterCount` counts the flat parameters: 2 for a string or a list, 1 for an enum or a 32-bit
/// integer, 3 for an `option<string>`, plus 1 for the address of a result that flattens to more than one
/// value. Every row's name and count is what `wit-bindgen c` generates for the WIT.
inline constexpr auto DaemonImportRows = std::array<DaemonImportRow, 25> {
    // clang-format off
    DaemonImportRow { DaemonImport::Log, "log", 3,
        "log: func(level: log-level, message: string)", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::Wake, "wake", 1,
        "wake: func(after-ms: u32)", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::Now, "now", 1,
        "now: func() -> clock-reading", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::ProcStart, "proc-start", 9,
        "proc-start: func(command: string, args: list<tuple<string, string>>, cwd: string, tag: string) -> result<proc-handle, host-error>",
        ImportAvailability::Served },
    DaemonImportRow { DaemonImport::Procs, "procs", 1,
        "procs: func() -> result<list<proc-info>, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::ProcTake, "proc-take", 4,
        "proc-take: func(handle: proc-handle, output: proc-stream, offset: u32) -> result<chunk, host-error>",
        ImportAvailability::Served },
    DaemonImportRow { DaemonImport::ProcRelease, "proc-release", 2,
        "proc-release: func(handle: proc-handle) -> result<_, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::StoreGet, "store-get", 3,
        "store-get: func(key: string) -> result<option<list<u8>>, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::StoreSet, "store-set", 5,
        "store-set: func(key: string, value: list<u8>) -> result<_, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::StoreRemove, "store-remove", 3,
        "store-remove: func(key: string) -> result<_, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::StoreKeys, "store-keys", 3,
        "store-keys: func(prefix: string) -> result<list<string>, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::PaneOpen, "pane-open", 14,
        "pane-open: func(template: string, args: list<tuple<string, string>>, cwd: string, title: string, tag: string, near: option<string>) -> result<string, host-error>",
        ImportAvailability::Served },
    DaemonImportRow { DaemonImport::PaneClose, "pane-close", 3,
        "pane-close: func(pane: string) -> result<_, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::Panes, "panes", 1,
        "panes: func() -> result<list<pane-info>, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::TabOpen, "tab-open", 4,
        "tab-open: func(scheme: string, origin: origin) -> result<string, host-error>",
        ImportAvailability::Served },
    DaemonImportRow { DaemonImport::TabClose, "tab-close", 3,
        "tab-close: func(tab: string) -> result<_, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::TabReveal, "tab-reveal", 4,
        "tab-reveal: func(tab: string, origin: origin) -> result<_, host-error>",
        ImportAvailability::Served },
    DaemonImportRow { DaemonImport::Tabs, "tabs", 1,
        "tabs: func() -> result<list<tab-info>, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::TabRead, "tab-read", 3,
        "tab-read: func(tab: string) -> result<list<u8>, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::TabWrite, "tab-write", 5,
        "tab-write: func(tab: string, bytes: list<u8>) -> result<_, host-error>",
        ImportAvailability::Served },
    DaemonImportRow { DaemonImport::ActionsTake, "actions-take", 1,
        "actions-take: func() -> result<list<action>, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::SummarySet, "summary-set", 5,
        "summary-set: func(short: string, detail: string) -> result<_, host-error>", ImportAvailability::Served },
    DaemonImportRow { DaemonImport::PaneReveal, "pane-reveal", 4,
        "pane-reveal: func(pane: string, origin: origin) -> result<_, host-error>",
        ImportAvailability::Served },
    DaemonImportRow { DaemonImport::FileRead, "file-read", 5,
        "file-read: func(root: string, path: string) -> result<file-content, file-error>",
        ImportAvailability::Served },
    DaemonImportRow { DaemonImport::PaneSend, "pane-send", 5,
        "pane-send: func(pane: string, text: string) -> result<_, host-error>",
        ImportAvailability::Served },
    // clang-format on
};

static_assert(DaemonImportRows.size() == std::to_underlying(DaemonImport::PaneSend) + 1U,
              "every DaemonImport has a row");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, DaemonImportRows.size()),
                                  [](std::size_t index) {
                                      return std::to_underlying(DaemonImportRows.at(index).import) == index;
                                  }),
              "DaemonImportRows is indexed by the enumerator's value");

/// The daemon world's host functions: one per row of DaemonImportRows, in its order, each under
/// DaemonImportModule, named and counted as its row says.
/// @return The table; it lives as long as the process.
[[nodiscard]] std::span<wasm::HostFunction const> daemonHostFunctions() noexcept;

} // namespace vtextension
