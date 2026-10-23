// SPDX-License-Identifier: Apache-2.0
#include <vtextension/CompiledExtension.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <vtextension/HostBindings.hpp>
#include <vtextension/wasm/WasiAllowList.hpp>

namespace vtextension
{

namespace
{
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

    /// Whether @p module (an import's module name) and @p function name a function the host provides:
    /// one of the world's host functions, or an allowed WASI function.
    [[nodiscard]] bool isProvided(std::string_view module, std::string_view function)
    {
        if (module == detail::HostImportModule)
            return true; // only what the host registered is satisfied, which the caller checks
        return module == wasm::WasiModule
               && std::ranges::find(wasm::AllowedWasiFunctions, function) != wasm::AllowedWasiFunctions.end();
    }

    /// Whether every import of @p module is a function the host provides and the runtime satisfies:
    /// the world's host functions (with the signatures the host registered) or an allowed WASI
    /// function. An unsatisfied one would trap only when called, if at all.
    [[nodiscard]] bool importsAreProvided(wasm::IWasmModule const& module)
    {
        return std::ranges::all_of(module.imports(), [](wasm::ImportDescription const& import) {
            return import.kind == wasm::ExternKind::Function && import.linkage == wasm::Linkage::Satisfied
                   && isProvided(import.module, import.name);
        });
    }

    /// Whether @p module exports what the world requires, and nothing the host calls in another shape.
    [[nodiscard]] bool exportsMatch(wasm::IWasmModule const& module)
    {
        auto const exports = module.exports();
        auto const find = [&](std::string_view name) {
            return std::ranges::find(exports, name, &wasm::ExportDescription::name);
        };

        auto const memory = find(MemoryExport);
        if (memory == exports.end() || memory->kind != wasm::ExternKind::Memory)
            return false;

        return std::ranges::all_of(ExportShapes, [&](ExportShape const& shape) {
            auto const entry = find(shape.name);
            if (entry == exports.end())
                return shape.presence == Presence::Optional;
            return entry->kind == wasm::ExternKind::Function && hasShape(entry->signature, shape);
        });
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

    // A start function would run during instantiation, which no deadline guards: the world has none,
    // and initialization belongs in a WASI reactor's `_initialize`, which is called under one.
    if (startsItself || !importsAreProvided(**module) || !exportsMatch(**module))
        return std::unexpected(ExtensionError::WorldMismatch);

    return std::make_shared<CompiledExtension>(PrivateTag {},
                                               engine.shared_from_this(),
                                               manifest ? std::optional { std::move(*manifest) }
                                                        : std::nullopt,
                                               std::move(*module));
}

} // namespace vtextension
