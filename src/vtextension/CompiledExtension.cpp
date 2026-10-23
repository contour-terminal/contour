// SPDX-License-Identifier: Apache-2.0
#include <vtextension/CompiledExtension.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <wasm_export.h>

#include <vtextension/HostBindings.hpp>

namespace vtextension
{

namespace
{
    /// The module name of WASI preview 1, the one import module besides the world's own.
    constexpr auto WasiModule = std::string_view { "wasi_snapshot_preview1" };

    /// The WASI preview 1 functions a guest may import. Each one returns at once and reaches nothing
    /// beyond the instance: with no preopened directory, no environment and standard streams bound
    /// to the null device, there is nothing for it to reach. Every other WASI function is refused,
    /// in particular `poll_oneoff`, whose clock wait the watchdog cannot interrupt (a guest times
    /// itself with `request-frame`, it never sleeps), and the `sock_*` and `path_*` families.
    constexpr auto AllowedWasiFunctions = std::to_array<std::string_view>({
        "args_get",            // the (empty) arguments
        "args_sizes_get",      //
        "environ_get",         // the (empty) environment; Rust's std reads it at start-up
        "environ_sizes_get",   //
        "clock_time_get",      // reads a clock; never waits
        "clock_res_get",       //
        "fd_write",            // to the null device
        "fd_close",            //
        "fd_fdstat_get",       // what a standard stream is; Rust's std asks before it prints
        "fd_prestat_get",      // finds no preopened directory
        "fd_prestat_dir_name", //
        "fd_seek",             // on the null device
        "random_get",          // fills guest memory; Rust's HashMap seeds itself with it
        "proc_exit",           // ends the call as a trap
        "sched_yield",         // returns at once in a single-threaded instance
    });

    /// Whether an export must be there.
    enum class Presence : uint8_t
    {
        Optional,
        Required,
    };

    /// One function export the world defines, with its Canonical ABI core signature: every
    /// parameter and result of this world flattens to i32.
    struct ExportShape
    {
        std::string_view name; ///< The export's name.
        uint32_t parameters;   ///< How many i32 parameters it takes.
        uint32_t results;      ///< How many i32 results it returns.
        Presence presence;     ///< Whether a module must export it.
    };

    /// The function exports the host calls. A post-return function exists only for an export whose
    /// result owns memory, and a module that is a WASI reactor exports its initializer.
    constexpr auto ExportShapes = std::array {
        ExportShape { .name = "load", .parameters = 2, .results = 1, .presence = Presence::Required },
        ExportShape { .name = "update", .parameters = 5, .results = 1, .presence = Presence::Required },
        ExportShape { .name = "render", .parameters = 2, .results = 1, .presence = Presence::Required },
        ExportShape { .name = "cabi_realloc", .parameters = 4, .results = 1, .presence = Presence::Required },
        ExportShape {
            .name = "cabi_post_load", .parameters = 1, .results = 0, .presence = Presence::Optional },
        ExportShape {
            .name = "cabi_post_update", .parameters = 1, .results = 0, .presence = Presence::Optional },
        ExportShape {
            .name = "cabi_post_render", .parameters = 1, .results = 0, .presence = Presence::Optional },
        ExportShape { .name = "_initialize", .parameters = 0, .results = 0, .presence = Presence::Optional },
    };

    /// The name the guest's linear memory is exported under.
    constexpr auto MemoryExport = std::string_view { "memory" };

    /// Whether every one of @p count values of @p kindAt is an i32.
    template <typename KindAt>
    [[nodiscard]] bool allI32(uint32_t count, KindAt const& kindAt)
    {
        return std::ranges::all_of(std::views::iota(uint32_t { 0 }, count),
                                   [&](uint32_t index) { return kindAt(index) == WASM_I32; });
    }

    /// Whether @p type has the shape @p shape describes.
    [[nodiscard]] bool hasShape(wasm_func_type_t type, ExportShape const& shape)
    {
        return wasm_func_type_get_param_count(type) == shape.parameters
               && wasm_func_type_get_result_count(type) == shape.results
               && allI32(shape.parameters,
                         [&](uint32_t index) { return wasm_func_type_get_param_valkind(type, index); })
               && allI32(shape.results,
                         [&](uint32_t index) { return wasm_func_type_get_result_valkind(type, index); });
    }

    /// Whether @p module (an import's module name) and @p function name a function the host provides:
    /// one of the world's host functions, or an allowed WASI function.
    [[nodiscard]] bool isProvided(std::string_view module, std::string_view function)
    {
        if (module == std::string_view { detail::HostImportModule })
            return true; // only what the host registered links, which the caller checks
        return module == WasiModule
               && std::ranges::find(AllowedWasiFunctions, function) != AllowedWasiFunctions.end();
    }

    /// Whether every import of @p module is a function the host provides and the runtime linked:
    /// the world's host functions (with the signatures the host registered) or an allowed WASI
    /// function. The runtime links imports lazily, so an unlinked one would trap only when called.
    [[nodiscard]] bool importsAreProvided(WASMModuleCommon* module)
    {
        auto const count = wasm_runtime_get_import_count(module);
        return std::ranges::all_of(std::views::iota(int32_t { 0 }, std::max(count, int32_t { 0 })),
                                   [&](int32_t index) {
                                       auto import = wasm_import_t {};
                                       wasm_runtime_get_import_type(module, index, &import);
                                       return import.kind == WASM_IMPORT_EXPORT_KIND_FUNC && import.linked
                                              && isProvided(import.module_name, import.name);
                                   });
    }

    /// Whether @p module exports what the world requires, and nothing the host calls in another shape.
    [[nodiscard]] bool exportsMatch(WASMModuleCommon* module)
    {
        auto const count = std::max(wasm_runtime_get_export_count(module), int32_t { 0 });
        auto exports = std::vector<wasm_export_t>(static_cast<std::size_t>(count));
        for (auto const index: std::views::iota(int32_t { 0 }, count))
            wasm_runtime_get_export_type(module, index, &exports.at(static_cast<std::size_t>(index)));

        auto const find = [&](std::string_view name) {
            return std::ranges::find_if(exports,
                                        [&](wasm_export_t const& entry) { return entry.name == name; });
        };

        auto const memory = find(MemoryExport);
        if (memory == exports.end() || memory->kind != WASM_IMPORT_EXPORT_KIND_MEMORY)
            return false;

        return std::ranges::all_of(ExportShapes, [&](ExportShape const& shape) {
            auto const entry = find(shape.name);
            if (entry == exports.end())
                return shape.presence == Presence::Optional;
            return entry->kind == WASM_IMPORT_EXPORT_KIND_FUNC && hasShape(entry->u.func_type, shape);
        });
    }
} // namespace

CompiledExtension::CompiledExtension(PrivateTag /*unused*/,
                                     std::shared_ptr<Engine> engine,
                                     std::vector<std::byte> bytes,
                                     std::optional<Manifest> manifest,
                                     wasm::wamr::detail::ModuleHandle module):
    _engine { std::move(engine) },
    _bytes { std::move(bytes) },
    _manifest { std::move(manifest) },
    _module { std::move(module) }
{
}

std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> CompiledExtension::load(
    Engine& engine, std::vector<std::byte> bytes)
{
    // The manifest is read before the runtime sees the bytes, which its loader may rewrite in place.
    auto manifest = parseManifest(bytes);
    auto const startsItself = hasStartFunction(bytes);

    if (bytes.size() > std::numeric_limits<uint32_t>::max())
        return std::unexpected(ExtensionError::InvalidComponent);
    auto errorBuffer = std::array<char, 128> {};
    // The vector's buffer stays where it is when the vector moves into the CompiledExtension.
    auto module =
        wasm::wamr::detail::ModuleHandle { wasm_runtime_load(reinterpret_cast<uint8_t*>(bytes.data()),
                                                             static_cast<uint32_t>(bytes.size()),
                                                             errorBuffer.data(),
                                                             static_cast<uint32_t>(errorBuffer.size())) };
    if (!module)
        return std::unexpected(ExtensionError::InvalidComponent);

    // A module that is one, but whose manifest does not parse, is refused before anything else. One
    // without a manifest declares no commands.
    if (!manifest && manifest.error() != ExtensionError::ManifestMissing)
        return std::unexpected(manifest.error());

    // A start function would run during instantiation, which no deadline guards: the world has none,
    // and initialization belongs in a WASI reactor's `_initialize`, which is called under one.
    if (startsItself || !importsAreProvided(module.get()) || !exportsMatch(module.get()))
        return std::unexpected(ExtensionError::WorldMismatch);

    // No directories, no environment, no arguments, and standard streams that lead nowhere: the
    // guest's standard library works, but reaches nothing outside the module. In particular nothing a
    // guest prints can reach the terminal the host runs in.
    auto const nullDevice = engine.nullDeviceHandle();
    wasm_runtime_set_wasi_args_ex(
        module.get(), nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullDevice, nullDevice, nullDevice);

    return std::make_shared<CompiledExtension>(PrivateTag {},
                                               engine.shared_from_this(),
                                               std::move(bytes),
                                               manifest ? std::optional { std::move(*manifest) }
                                                        : std::nullopt,
                                               std::move(module));
}

} // namespace vtextension
