// SPDX-License-Identifier: Apache-2.0
#include <vtextension/CompiledExtension.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <vtextension/HostBindings.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/World.hpp>
#include <vtextension/wasm/WasiAllowList.hpp>

namespace vtextension
{

namespace
{
    /// Whether an export must be there.
    enum class Presence : uint8_t
    {
        Optional,  ///< A module may export it.
        Required,  ///< A module must export it.
        Forbidden, ///< A module must not: it is another world's, and the host would never call it.
    };

    /// One function export a world defines, with its Canonical ABI core signature: every
    /// parameter and result of these worlds flattens to i32.
    struct ExportShape
    {
        std::string_view name; ///< The export's name.
        uint32_t parameters;   ///< How many i32 parameters it takes.
        uint32_t results;      ///< How many i32 results it returns.
        Presence presence;     ///< Whether a module must export it.
    };

    /// The status-line world's function exports. A post-return function exists only for an export whose
    /// result owns memory, and a module that is a WASI reactor exports its initializer, which the
    /// host calls once, under the load deadline (wasm::IWasmModule::instantiate).
    constexpr auto StatusLineExportShapes = std::array {
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

    /// The daemon world's function exports: `load(config-json, host-info)` and `update(hint)` return
    /// nothing, so neither has a post-return function. A `render` is the status line's: a module exporting
    /// one was built for the other world.
    constexpr auto DaemonExportShapes = std::array {
        ExportShape { .name = "load", .parameters = 9, .results = 0, .presence = Presence::Required },
        ExportShape { .name = "update", .parameters = 1, .results = 0, .presence = Presence::Required },
        ExportShape { .name = "cabi_realloc", .parameters = 4, .results = 1, .presence = Presence::Required },
        ExportShape { .name = "_initialize", .parameters = 0, .results = 0, .presence = Presence::Optional },
        ExportShape { .name = "render", .parameters = 0, .results = 0, .presence = Presence::Forbidden },
    };

    /// What a module of one world is: the placement its manifest declares, and its exports.
    struct WorldShape
    {
        World world;                          ///< The world described.
        ManifestPlacement placement;          ///< The placement its modules' manifests declare.
        std::span<ExportShape const> exports; ///< The function exports the host calls.
    };

    /// Every world's shape, indexed by World.
    constexpr auto WorldShapes = std::array {
        WorldShape { .world = World::StatusLine,
                     .placement = ManifestPlacement::StatusLine,
                     .exports = StatusLineExportShapes },
        WorldShape {
            .world = World::Daemon, .placement = ManifestPlacement::Daemon, .exports = DaemonExportShapes },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, WorldShapes.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(WorldShapes.at(index).world) == index;
                                      }),
                  "WorldShapes is indexed by the World's value");

    /// What a diagnostic calls each world, indexed by World.
    constexpr auto WorldNames =
        std::array { std::string_view { "status-line" }, std::string_view { "daemon" } };

    /// The name the guest's linear memory is exported under.
    constexpr auto MemoryExport = std::string_view { "memory" };

    /// Whether @p types is exactly @p count i32s.
    [[nodiscard]] bool areI32s(std::span<wasm::ValueType const> types, uint32_t count) noexcept
    {
        return types.size() == count && std::ranges::all_of(types, [](wasm::ValueType type) {
                   return type == wasm::ValueType::I32;
               });
    }

    /// Whether @p signature has the shape @p shape describes.
    [[nodiscard]] bool hasShape(wasm::FunctionSignature const& signature, ExportShape const& shape) noexcept
    {
        return areI32s(signature.parameters, shape.parameters) && areI32s(signature.results, shape.results);
    }

    /// Whether @p import names a function a module of @p world may import: one of the world's exact
    /// (module, name) pairs, or an allowed WASI function.
    [[nodiscard]] bool isProvided(World world, wasm::ImportDescription const& import)
    {
        if (std::ranges::contains(detail::worldImports(world),
                                  detail::WorldImport { .module = import.module, .name = import.name }))
            return true;
        return import.module == wasm::WasiModule
               && std::ranges::contains(wasm::AllowedWasiFunctions, import.name);
    }

    /// Finds an import of @p module that is not a function of @p world the runtime satisfies: one of the
    /// world's host functions (with the signature the host registered) or an allowed WASI function. An
    /// unsatisfied one would trap only when called, if at all; a satisfied one of another world may be
    /// linked to that world's registration in a process-wide registry, and is refused all the same.
    /// @return The first such import as `module/name`; std::nullopt if there is none.
    [[nodiscard]] std::optional<std::string> foreignImport(World world, wasm::IWasmModule const& module)
    {
        auto const imports = module.imports();
        auto const found = std::ranges::find_if(imports, [world](wasm::ImportDescription const& import) {
            return import.kind != wasm::ExternKind::Function || import.linkage != wasm::Linkage::Satisfied
                   || !isProvided(world, import);
        });
        if (found == imports.end())
            return std::nullopt;
        return std::format("import {}/{}", found->module, found->name);
    }

    /// Finds what keeps @p module from exporting what @p shapes require: a missing memory, a required
    /// export missing, a forbidden one present, or one the host calls exported in another shape.
    /// @return What it is; std::nullopt if the exports match.
    [[nodiscard]] std::optional<std::string> mismatchedExport(std::span<ExportShape const> shapes,
                                                              wasm::IWasmModule const& module)
    {
        auto const exports = module.exports();
        auto const find = [&](std::string_view name) {
            return std::ranges::find(exports, name, &wasm::ExportDescription::name);
        };

        auto const memory = find(MemoryExport);
        if (memory == exports.end() || memory->kind != wasm::ExternKind::Memory)
            return std::format("export {}", MemoryExport);

        auto const mismatch = std::ranges::find_if(shapes, [&](ExportShape const& shape) {
            auto const entry = find(shape.name);
            if (entry == exports.end())
                return shape.presence == Presence::Required;
            return shape.presence == Presence::Forbidden || entry->kind != wasm::ExternKind::Function
                   || !hasShape(entry->signature, shape);
        });
        if (mismatch == shapes.end())
            return std::nullopt;
        return std::format("export {}", mismatch->name);
    }

    /// Finds why a module does not speak @p shape's world.
    /// @return What is wrong; std::nullopt if it does.
    [[nodiscard]] std::optional<std::string> worldMismatch(WorldShape const& shape,
                                                           wasm::IWasmModule const& module,
                                                           ManifestPlacement placement)
    {
        if (placement != shape.placement)
            return "the manifest's placement";
        if (auto foreign = foreignImport(shape.world, module))
            return foreign;
        return mismatchedExport(shape.exports, module);
    }
} // namespace

CompiledExtension::CompiledExtension(PrivateTag /*unused*/,
                                     std::shared_ptr<Engine> engine,
                                     std::optional<Manifest> manifest,
                                     std::shared_ptr<wasm::IWasmModule> module):
    _engine { std::move(engine) }, _manifest { std::move(manifest) }, _module { std::move(module) }
{
}

std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> CompiledExtension::load(
    Engine& engine, std::vector<std::byte> bytes)
{
    // The manifest is read before the runtime sees the bytes, which its loader may rewrite in place.
    auto manifest = parseManifest(bytes);
    auto const startsItself = hasStartFunction(bytes);

    auto module = engine.runtime().load(std::move(bytes));
    if (!module)
        return std::unexpected(ExtensionError::InvalidComponent);

    // A module that is one, but whose manifest does not parse, is refused before anything else. One
    // without a manifest declares no commands.
    if (!manifest && manifest.error() != ExtensionError::ManifestMissing)
        return std::unexpected(manifest.error());

    // A start function would run during instantiation, which no deadline guards: no world has one,
    // and initialization belongs in a WASI reactor's `_initialize`, which is called under one. A module
    // without a manifest is a status-line extension's, as one without a `placement` line is.
    auto const& shape = WorldShapes.at(std::to_underlying(engine.world()));
    auto const placement = manifest ? manifest->placement : ManifestPlacement::StatusLine;
    auto const mismatch = startsItself ? std::optional<std::string> { "a start function" }
                                       : worldMismatch(shape, **module, placement);
    if (mismatch)
    {
        errorLog()("An extension module does not speak the {} world: {}.",
                   WorldNames.at(std::to_underlying(shape.world)),
                   *mismatch);
        return std::unexpected(ExtensionError::WorldMismatch);
    }

    return std::make_shared<CompiledExtension>(PrivateTag {},
                                               engine.shared_from_this(),
                                               manifest ? std::optional { std::move(*manifest) }
                                                        : std::nullopt,
                                               std::move(*module));
}

} // namespace vtextension
