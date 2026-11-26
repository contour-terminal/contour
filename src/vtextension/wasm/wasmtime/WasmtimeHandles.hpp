// SPDX-License-Identifier: Apache-2.0
//
// Owning handles on Wasmtime's opaque objects, so that headers need not pull wasmtime.h in.
#pragma once

#include <memory>

struct wasm_engine_t;
struct wasm_functype_t;
struct wasm_trap_t;
struct wasmtime_error;
struct wasmtime_linker;
struct wasmtime_module;
struct wasmtime_store;

namespace vtextension::wasm::wasmtime::detail
{

/// Deletes an engine.
struct EngineDeleter
{
    /// Deletes @p engine.
    void operator()(wasm_engine_t* engine) const noexcept;
};

/// Deletes a store, and every instance in it.
struct StoreDeleter
{
    /// Deletes @p store.
    void operator()(wasmtime_store* store) const noexcept;
};

/// Deletes a compiled module.
struct ModuleDeleter
{
    /// Deletes @p module.
    void operator()(wasmtime_module* module) const noexcept;
};

/// Deletes a linker.
struct LinkerDeleter
{
    /// Deletes @p linker.
    void operator()(wasmtime_linker* linker) const noexcept;
};

/// Deletes an error.
struct ErrorDeleter
{
    /// Deletes @p error.
    void operator()(wasmtime_error* error) const noexcept;
};

/// Deletes a trap.
struct TrapDeleter
{
    /// Deletes @p trap.
    void operator()(wasm_trap_t* trap) const noexcept;
};

/// Deletes a function type.
struct FuncTypeDeleter
{
    /// Deletes @p type.
    void operator()(wasm_functype_t* type) const noexcept;
};

/// An engine: the compiler and its configuration.
using EngineHandle = std::unique_ptr<wasm_engine_t, EngineDeleter>;

/// A store: what instances live in.
using StoreHandle = std::unique_ptr<wasmtime_store, StoreDeleter>;

/// A compiled module.
using ModuleHandle = std::unique_ptr<wasmtime_module, ModuleDeleter>;

/// A linker: the host functions modules are instantiated against.
using LinkerHandle = std::unique_ptr<wasmtime_linker, LinkerDeleter>;

/// An error a Wasmtime call returned; null for none.
using ErrorHandle = std::unique_ptr<wasmtime_error, ErrorDeleter>;

/// A trap.
using TrapHandle = std::unique_ptr<wasm_trap_t, TrapDeleter>;

/// A function type.
using FuncTypeHandle = std::unique_ptr<wasm_functype_t, FuncTypeDeleter>;

} // namespace vtextension::wasm::wasmtime::detail
