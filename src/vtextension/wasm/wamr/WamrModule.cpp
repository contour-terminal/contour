// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <wasm_export.h>

#include <vtextension/wasm/wamr/WamrHandles.hpp>
#include <vtextension/wasm/wamr/WamrRuntime.hpp>

namespace vtextension::wasm::wamr
{

namespace detail
{
    void ModuleUnloader::operator()(WASMModuleCommon* module) const noexcept
    {
        wasm_runtime_unload(module);
    }

    void InstanceDeinstantiator::operator()(WASMModuleInstanceCommon* instance) const noexcept
    {
        wasm_runtime_deinstantiate(instance);
    }

    void ExecEnvDestroyer::operator()(WASMExecEnv* execEnv) const noexcept
    {
        wasm_runtime_destroy_exec_env(execEnv);
    }
} // namespace detail

namespace
{
    /// One row of the value-type table: a WAMR value kind and the neutral type it is.
    struct ValueKindRow
    {
        wasm_valkind_t kind; ///< WAMR's value kind.
        ValueType type;      ///< The neutral value type.
    };

    /// The value kinds the interface names; every other kind is ValueType::Other.
    constexpr auto ValueKinds = std::array {
        ValueKindRow { .kind = WASM_I32, .type = ValueType::I32 },
        ValueKindRow { .kind = WASM_I64, .type = ValueType::I64 },
        ValueKindRow { .kind = WASM_F32, .type = ValueType::F32 },
        ValueKindRow { .kind = WASM_F64, .type = ValueType::F64 },
    };

    /// The neutral extern kinds, indexed by WAMR's import/export kind.
    constexpr auto ExternKinds = std::array {
        ExternKind::Function, // WASM_IMPORT_EXPORT_KIND_FUNC
        ExternKind::Table,    // WASM_IMPORT_EXPORT_KIND_TABLE
        ExternKind::Memory,   // WASM_IMPORT_EXPORT_KIND_MEMORY
        ExternKind::Global,   // WASM_IMPORT_EXPORT_KIND_GLOBAL
    };
    static_assert(WASM_IMPORT_EXPORT_KIND_FUNC == 0 && WASM_IMPORT_EXPORT_KIND_TABLE == 1
                      && WASM_IMPORT_EXPORT_KIND_MEMORY == 2 && WASM_IMPORT_EXPORT_KIND_GLOBAL == 3,
                  "ExternKinds is indexed by WAMR's import/export kind");

    /// @return The neutral type of WAMR's value kind @p kind.
    [[nodiscard]] ValueType valueTypeOf(wasm_valkind_t kind) noexcept
    {
        auto const row = std::ranges::find(ValueKinds, kind, &ValueKindRow::kind);
        return row != ValueKinds.end() ? row->type : ValueType::Other;
    }

    /// @return The neutral kind of WAMR's import/export kind @p kind.
    [[nodiscard]] ExternKind externKindOf(wasm_import_export_kind_t kind)
    {
        return ExternKinds.at(static_cast<std::size_t>(kind));
    }

    /// @return The @p count value types @p kindAt names, in order.
    template <typename KindAt>
    [[nodiscard]] std::vector<ValueType> valueTypes(uint32_t count, KindAt const& kindAt)
    {
        return std::ranges::to<std::vector>(
            std::views::iota(uint32_t { 0 }, count)
            | std::views::transform([&](uint32_t index) { return valueTypeOf(kindAt(index)); }));
    }

    /// @return The signature of WAMR's function type @p type.
    [[nodiscard]] FunctionSignature signatureOf(wasm_func_type_t type)
    {
        return FunctionSignature {
            .parameters =
                valueTypes(wasm_func_type_get_param_count(type),
                           [&](uint32_t index) { return wasm_func_type_get_param_valkind(type, index); }),
            .results =
                valueTypes(wasm_func_type_get_result_count(type),
                           [&](uint32_t index) { return wasm_func_type_get_result_valkind(type, index); }),
        };
    }

    /// @return The indices 0 to @p count (WAMR's count of imports or exports; -1 on failure, so none).
    [[nodiscard]] auto indicesUpTo(int32_t count)
    {
        return std::views::iota(int32_t { 0 }, std::max(count, int32_t { 0 }));
    }

    /// @return The imports of @p module, each with whether WAMR linked it: it does so at load time,
    ///         against the registered natives and their signatures, and an unlinked import would trap
    ///         only when called.
    [[nodiscard]] std::vector<ImportDescription> describeImports(WASMModuleCommon* module)
    {
        return std::ranges::to<std::vector>(
            indicesUpTo(wasm_runtime_get_import_count(module)) | std::views::transform([&](int32_t index) {
                auto import = wasm_import_t {};
                wasm_runtime_get_import_type(module, index, &import);
                auto const kind = externKindOf(import.kind);
                return ImportDescription {
                    .module = std::string { import.module_name },
                    .name = std::string { import.name },
                    .kind = kind,
                    .signature =
                        kind == ExternKind::Function ? signatureOf(import.u.func_type) : FunctionSignature {},
                    .linkage = import.linked ? Linkage::Satisfied : Linkage::Unsatisfied,
                };
            }));
    }

    /// @return The exports of @p module.
    [[nodiscard]] std::vector<ExportDescription> describeExports(WASMModuleCommon* module)
    {
        return std::ranges::to<std::vector>(
            indicesUpTo(wasm_runtime_get_export_count(module)) | std::views::transform([&](int32_t index) {
                auto entry = wasm_export_t {};
                wasm_runtime_get_export_type(module, index, &entry);
                auto const kind = externKindOf(entry.kind);
                return ExportDescription {
                    .name = std::string { entry.name },
                    .kind = kind,
                    .signature =
                        kind == ExternKind::Function ? signatureOf(entry.u.func_type) : FunctionSignature {},
                    .initialPages = kind == ExternKind::Memory
                                        ? wasm_memory_type_get_init_page_count(entry.u.memory_type)
                                        : 0,
                };
            }));
    }

    /// A module loaded into WAMR.
    class WamrModule final: public IWasmModule, public std::enable_shared_from_this<WamrModule>
    {
      public:
        WamrModule(std::shared_ptr<WamrRuntime const> runtime,
                   std::vector<std::byte> bytes,
                   detail::ModuleHandle module,
                   std::vector<ImportDescription> imports,
                   std::vector<ExportDescription> exports) noexcept:
            _runtime { std::move(runtime) },
            _bytes { std::move(bytes) },
            _module { std::move(module) },
            _imports { std::move(imports) },
            _exports { std::move(exports) }
        {
        }

        [[nodiscard]] std::span<ImportDescription const> imports() const noexcept override
        {
            return _imports;
        }

        [[nodiscard]] std::span<ExportDescription const> exports() const noexcept override
        {
            return _exports;
        }

        [[nodiscard]] std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> instantiate(
            IHostContext& context) override
        {
            return instantiateModule(
                shared_from_this(), _module.get(), _runtime->limits(), _exports, context);
        }

      private:
        std::shared_ptr<WamrRuntime const> _runtime; ///< Declared first: released after the module.
        std::vector<std::byte> _bytes;               ///< Must outlive the module, which refers into it.
        detail::ModuleHandle _module;                ///< Unloaded before the bytes it refers into.
        std::vector<ImportDescription> _imports;
        std::vector<ExportDescription> _exports;
    };
} // namespace

std::expected<std::shared_ptr<IWasmModule>, ExtensionError> loadModule(
    std::shared_ptr<WamrRuntime const> runtime, std::vector<std::byte> bytes)
{
    if (bytes.size() > std::numeric_limits<uint32_t>::max())
        return std::unexpected(ExtensionError::InvalidComponent);
    auto errorBuffer = std::array<char, 128> {};
    // The vector's buffer stays where it is when the vector moves into the module. WAMR may rewrite it.
    auto module = detail::ModuleHandle { wasm_runtime_load(reinterpret_cast<uint8_t*>(bytes.data()),
                                                           static_cast<uint32_t>(bytes.size()),
                                                           errorBuffer.data(),
                                                           static_cast<uint32_t>(errorBuffer.size())) };
    if (!module)
        return std::unexpected(ExtensionError::InvalidComponent);

    // No directories, no environment, no arguments, and standard streams that lead nowhere: the
    // guest's standard library works, but reaches nothing outside the module. WAMR applies these per
    // module, at instantiation.
    auto const nullDevice = runtime->nullDevice();
    wasm_runtime_set_wasi_args_ex(
        module.get(), nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullDevice, nullDevice, nullDevice);

    auto imports = describeImports(module.get());
    auto exports = describeExports(module.get());
    return std::make_shared<WamrModule>(
        std::move(runtime), std::move(bytes), std::move(module), std::move(imports), std::move(exports));
}

} // namespace vtextension::wasm::wamr
