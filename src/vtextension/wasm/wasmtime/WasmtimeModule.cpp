// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wasmtime.h>

#include <vtextension/wasm/WasiAllowList.hpp>
#include <vtextension/wasm/WasmBinary.hpp>
#include <vtextension/wasm/wasi/Preview1Signatures.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeHandles.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeRuntime.hpp>

namespace vtextension::wasm::wasmtime
{

namespace detail
{
    void EngineDeleter::operator()(wasm_engine_t* engine) const noexcept
    {
        wasm_engine_delete(engine);
    }

    void StoreDeleter::operator()(wasmtime_store* store) const noexcept
    {
        wasmtime_store_delete(store);
    }

    void ModuleDeleter::operator()(wasmtime_module* module) const noexcept
    {
        wasmtime_module_delete(module);
    }

    void LinkerDeleter::operator()(wasmtime_linker* linker) const noexcept
    {
        wasmtime_linker_delete(linker);
    }

    void ErrorDeleter::operator()(wasmtime_error* error) const noexcept
    {
        wasmtime_error_delete(error);
    }

    void TrapDeleter::operator()(wasm_trap_t* trap) const noexcept
    {
        wasm_trap_delete(trap);
    }

    void FuncTypeDeleter::operator()(wasm_functype_t* type) const noexcept
    {
        wasm_functype_delete(type);
    }
} // namespace detail

namespace
{
    /// A vector the Wasmtime C API fills, deleted with it.
    template <typename Vec, void (*Delete)(Vec*)>
    class OwnedVec
    {
      public:
        OwnedVec() = default;
        OwnedVec(OwnedVec const&) = delete;
        OwnedVec(OwnedVec&&) = delete;
        OwnedVec& operator=(OwnedVec const&) = delete;
        OwnedVec& operator=(OwnedVec&&) = delete;
        ~OwnedVec() { Delete(&_vec); }

        /// @return The vector, for the C API to fill.
        [[nodiscard]] Vec* out() noexcept { return &_vec; }

        /// @return Its elements.
        [[nodiscard]] auto elements() const noexcept { return std::span { _vec.data, _vec.size }; }

      private:
        Vec _vec {};
    };

    using ImportTypes = OwnedVec<wasm_importtype_vec_t, &wasm_importtype_vec_delete>;
    using ExportTypes = OwnedVec<wasm_exporttype_vec_t, &wasm_exporttype_vec_delete>;

    /// One row of the value-type table: a Wasmtime value kind and the neutral type it is.
    struct ValueKindRow
    {
        wasm_valkind_t kind; ///< Wasmtime's value kind.
        ValueType type;      ///< The neutral value type.
    };

    /// The value kinds the interface names; every other kind is ValueType::Other.
    constexpr auto ValueKinds = std::array {
        ValueKindRow { .kind = WASM_I32, .type = ValueType::I32 },
        ValueKindRow { .kind = WASM_I64, .type = ValueType::I64 },
        ValueKindRow { .kind = WASM_F32, .type = ValueType::F32 },
        ValueKindRow { .kind = WASM_F64, .type = ValueType::F64 },
    };

    /// One row of the extern-kind table: a Wasmtime extern kind and the neutral kind it is.
    struct ExternKindRow
    {
        wasm_externkind_t kind; ///< Wasmtime's extern kind.
        ExternKind neutral;     ///< The neutral extern kind.
    };

    /// The extern kinds the interface names. A tag (WASM_EXTERN_TAG) needs exceptions, which every
    /// engine refuses, so no loaded module has one.
    constexpr auto ExternKinds = std::array {
        ExternKindRow { .kind = WASM_EXTERN_FUNC, .neutral = ExternKind::Function },
        ExternKindRow { .kind = WASM_EXTERN_GLOBAL, .neutral = ExternKind::Global },
        ExternKindRow { .kind = WASM_EXTERN_TABLE, .neutral = ExternKind::Table },
        ExternKindRow { .kind = WASM_EXTERN_MEMORY, .neutral = ExternKind::Memory },
    };

    /// @return The text of a Wasmtime name.
    [[nodiscard]] std::string textOf(wasm_name_t const* name)
    {
        return std::string { name->data, name->size };
    }

    /// @return The neutral types of @p types, in order.
    [[nodiscard]] std::vector<ValueType> valueTypes(wasm_valtype_vec_t const* types)
    {
        return std::ranges::to<std::vector>(
            std::span { types->data, types->size } | std::views::transform([](wasm_valtype_t const* type) {
                auto const row = std::ranges::find(ValueKinds, wasm_valtype_kind(type), &ValueKindRow::kind);
                return row != ValueKinds.end() ? row->type : ValueType::Other;
            }));
    }

    /// What an import or export is, as the interface describes it.
    struct ExternShape
    {
        ExternKind kind = ExternKind::Function; ///< What it is.
        FunctionSignature signature;            ///< The function's signature; empty unless a function.
        uint32_t initialPages = 0;              ///< The memory's initial size; 0 unless a memory.
    };

    /// @return What @p type is; nothing if the interface has no name for it.
    [[nodiscard]] std::optional<ExternShape> shapeOf(wasm_externtype_t const* type)
    {
        auto const row = std::ranges::find(ExternKinds, wasm_externtype_kind(type), &ExternKindRow::kind);
        if (row == ExternKinds.end())
            return std::nullopt;
        auto shape = ExternShape { .kind = row->neutral, .signature = {}, .initialPages = 0 };
        if (auto const* const function = wasm_externtype_as_functype_const(type))
            shape.signature = FunctionSignature { .parameters = valueTypes(wasm_functype_params(function)),
                                                  .results = valueTypes(wasm_functype_results(function)) };
        if (auto const* const memory = wasm_externtype_as_memorytype_const(type))
            shape.initialPages = static_cast<uint32_t>(wasmtime_memorytype_minimum(memory));
        return shape;
    }

    /// @return The imports of @p module, each with whether @p runtime provides it; nothing if one of them
    ///         is of a kind the interface has no name for.
    [[nodiscard]] std::optional<std::vector<ImportDescription>> describeImports(
        wasmtime_module_t const* module, WasmtimeRuntime const& runtime)
    {
        auto types = ImportTypes {};
        wasmtime_module_imports(module, types.out());
        auto imports = std::vector<ImportDescription> {};
        imports.reserve(types.elements().size());
        for (auto const* const type: types.elements())
        {
            auto const shape = shapeOf(wasm_importtype_type(type));
            if (!shape)
                return std::nullopt;
            auto& import =
                imports.emplace_back(ImportDescription { .module = textOf(wasm_importtype_module(type)),
                                                         .name = textOf(wasm_importtype_name(type)),
                                                         .kind = shape->kind,
                                                         .signature = shape->signature });
            import.linkage = linkageOf(import, runtime.hostBindings());
        }
        return imports;
    }

    /// @return The exports of @p module; nothing if one of them is of a kind the interface has no name for.
    [[nodiscard]] std::optional<std::vector<ExportDescription>> describeExports(
        wasmtime_module_t const* module)
    {
        auto types = ExportTypes {};
        wasmtime_module_exports(module, types.out());
        auto exports = std::vector<ExportDescription> {};
        exports.reserve(types.elements().size());
        for (auto const* const type: types.elements())
        {
            auto const shape = shapeOf(wasm_exporttype_type(type));
            if (!shape)
                return std::nullopt;
            exports.push_back(ExportDescription { .name = textOf(wasm_exporttype_name(type)),
                                                  .kind = shape->kind,
                                                  .signature = shape->signature,
                                                  .initialPages = shape->initialPages });
        }
        return exports;
    }
} // namespace

Linkage linkageOf(ImportDescription const& import, std::span<HostBinding const> hostBindings)
{
    if (import.kind != ExternKind::Function)
        return Linkage::Unsatisfied;
    if (import.module == WasiModule)
    {
        auto const* const function = wasi::findPreview1Function(import.name);
        return function && function->signature() == import.signature ? Linkage::Satisfied
                                                                     : Linkage::Unsatisfied;
    }
    auto const provides = [&](HostBinding const& binding) {
        return binding.module == import.module && binding.name == import.name
               && import.signature.results.empty()
               && import.signature.parameters.size() == binding.parameterCount
               && std::ranges::all_of(import.signature.parameters,
                                      [](ValueType type) { return type == ValueType::I32; });
    };
    return std::ranges::any_of(hostBindings, provides) ? Linkage::Satisfied : Linkage::Unsatisfied;
}

detail::FuncTypeHandle functionType(FunctionSignature const& signature)
{
    auto const fill = [](wasm_valtype_vec_t& vector, std::span<ValueType const> types) {
        wasm_valtype_vec_new_uninitialized(&vector, types.size());
        auto const slots = std::span { vector.data, vector.size };
        for (auto const [index, type]: core::views::enumerate(types))
        {
            auto const row = std::ranges::find(ValueKinds, type, &ValueKindRow::type);
            Require(row != ValueKinds.end());
            slots[static_cast<std::size_t>(index)] = wasm_valtype_new(row->kind);
        }
    };
    auto parameters = wasm_valtype_vec_t {};
    fill(parameters, signature.parameters);
    auto results = wasm_valtype_vec_t {};
    fill(results, signature.results);
    // wasm_functype_new takes both vectors over.
    return detail::FuncTypeHandle { wasm_functype_new(&parameters, &results) };
}

std::expected<std::shared_ptr<IWasmModule>, ExtensionError> loadModule(
    std::shared_ptr<WasmtimeRuntime> runtime, wasm_engine_t* engine, std::span<std::byte const> bytes)
{
    // More than MaxTables is refused before anything is compiled; Wasmtime would compile it.
    auto const tables = binary::tableDeclarations(bytes, MaxTables);
    if (!tables)
        return std::unexpected(ExtensionError::InvalidComponent);
    auto* compiled = static_cast<wasmtime_module_t*>(nullptr);
    auto const error = detail::ErrorHandle { wasmtime_module_new(
        engine, reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size(), &compiled) };
    auto module = detail::ModuleHandle { compiled };
    if (error || !module)
        return std::unexpected(ExtensionError::InvalidComponent);
    auto imports = describeImports(module.get(), *runtime);
    auto exports = describeExports(module.get());
    // Wasmtime's C API describes no memory a module does not export; its bytes do.
    auto const largestInitialPages = binary::largestInitialMemoryPages(bytes);
    if (!imports || !exports || !largestInitialPages)
        return std::unexpected(ExtensionError::InvalidComponent);
    return std::make_shared<WasmtimeModule>(std::move(runtime),
                                            std::move(module),
                                            std::move(*imports),
                                            std::move(*exports),
                                            *largestInitialPages,
                                            binary::largestInitialElements(*tables));
}

WasmtimeModule::WasmtimeModule(std::shared_ptr<WasmtimeRuntime> runtime,
                               detail::ModuleHandle module,
                               std::vector<ImportDescription> imports,
                               std::vector<ExportDescription> exports,
                               uint32_t largestInitialPages,
                               uint32_t largestInitialTableElements) noexcept:
    _runtime { std::move(runtime) },
    _module { std::move(module) },
    _imports { std::move(imports) },
    _exports { std::move(exports) },
    _largestInitialPages { largestInitialPages },
    _largestInitialTableElements { largestInitialTableElements }
{
}

} // namespace vtextension::wasm::wasmtime
