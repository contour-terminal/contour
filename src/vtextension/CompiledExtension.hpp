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
/// its engine's world:
///
/// - **Imports.** Every import is one of the world's exact (module, name) pairs (detail::worldImports())
///   or an allowed WASI preview 1 function, and the runtime satisfies it. The status line's are its
///   `host` functions and the control interface's verbs; the daemon's are its `host` functions alone.
/// - **Exports.** The status line's `load`, `update`, `render` and `cabi_realloc`; the daemon's `load`,
///   `update` and `cabi_realloc`, and no `render`. Each has the shape the Canonical ABI gives it, and both
///   export their memory.
/// - **Placement.** The manifest's `placement` is the world's: `daemon` exactly on a daemon engine.
/// - **No start function:** one would run during instantiation, where no deadline can guard it.
///
/// The manifest is optional for the status line. A module without one is an extension that declares no
/// commands, which therefore can never run one; a module whose manifest does not parse does not load. A
/// daemon-world module carries one: without it, it declares no placement, which is the status line's.
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
    ///         something the engine's world does not provide, lacks an export the world requires, has one
    ///         it forbids, or declares another world's placement.
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
