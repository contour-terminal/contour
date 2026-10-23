// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// An extension's module, loaded and checked once, from which any number of instances are created:
/// after a fault the host re-instantiates without reading or checking the module again.
///
/// Loading runs none of the module's code. It reads the manifest, and checks that the module speaks
/// the statusline world: every import it declares is one the host provides (the world's `host`
/// functions, or WASI preview 1), and it exports `load`, `update`, `render`, `cabi_realloc` and its
/// memory, each with the shape the Canonical ABI gives it. It has no start function: one would run
/// during instantiation, where no deadline can guard it.
///
/// The manifest is optional. A module without one is an extension that declares no commands, which
/// therefore can never run one; a module whose manifest does not parse does not load.
class CompiledExtension: public std::enable_shared_from_this<CompiledExtension>
{
    struct PrivateTag
    {
    };

  public:
    /// Loads and checks a module.
    /// @param engine The engine the module is loaded on and its instances run on; the module keeps it
    ///        alive.
    /// @param bytes The module's bytes; the runtime's module keeps them.
    /// @return The loaded module; `InvalidComponent` if it is not a valid core module,
    ///         `ManifestInvalid` if its manifest does not parse, `WorldMismatch` if it imports
    ///         something the host does not provide or lacks an export the world requires.
    [[nodiscard]] static std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> load(
        Engine& engine, std::vector<std::byte> bytes);

    /// Use load(); public only so that std::make_shared can reach it.
    CompiledExtension(PrivateTag,
                      std::shared_ptr<Engine> engine,
                      std::optional<Manifest> manifest,
                      std::shared_ptr<wasm::IWasmModule> module);

    ~CompiledExtension() = default;

    CompiledExtension(CompiledExtension const&) = delete;
    CompiledExtension(CompiledExtension&&) = delete;
    CompiledExtension& operator=(CompiledExtension const&) = delete;
    CompiledExtension& operator=(CompiledExtension&&) = delete;

    /// @return The manifest the module embeds; `std::nullopt` for a module that embeds none.
    [[nodiscard]] std::optional<Manifest> const& manifest() const noexcept { return _manifest; }

    /// @return The engine the module's instances run on.
    [[nodiscard]] Engine& engine() const noexcept { return *_engine; }

    /// @return The loaded runtime module, for instantiation.
    [[nodiscard]] wasm::IWasmModule& module() const noexcept { return *_module; }

  private:
    std::shared_ptr<Engine> _engine;
    std::optional<Manifest> _manifest;
    std::shared_ptr<wasm::IWasmModule> _module;
};

} // namespace vtextension
