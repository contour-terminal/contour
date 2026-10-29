// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the contract every WebAssembly backend must meet, written against the interface alone.
// A backend's test file defines a traits type with `static std::unique_ptr<wasm::IWasmBackend> make();`
// and instantiates the suite with CONTOUR_WASM_BACKEND_CONTRACT(Traits); every case is tagged
// `[wasm-contract]`. The modules are embedded with the WebAssembly text they were assembled from
// (`wat2wasm`), as in TestModules.hpp.
#pragma once

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <csignal>
#endif

#include <core/Utils.hpp>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/testing/StandardStreamsCapture.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/WasiAllowList.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm::testing::contract
{

/// The import module the contract's host functions are provided under.
inline constexpr auto HostModule = std::string_view { "contract:test/host@0" };

/// The host functions every contract runtime is created with: `ping`, taking one i32, and `pair`,
/// taking two.
inline constexpr auto HostFunctions = std::array {
    HostFunction { .module = HostModule, .name = "ping", .parameterCount = 1 },
    HostFunction { .module = HostModule, .name = "pair", .parameterCount = 2 },
};

/// The limits every contract runtime is created with.
inline constexpr auto Limits =
    RuntimeLimits { .memoryBytes = std::size_t { 4 } * 1024 * 1024, .stackBytes = std::size_t { 64 } * 1024 };

// Every backend must provide each WASI function the extension host allows: wasm::AllowedWasiFunctions,
// under wasm::WasiModule (WasiAllowList.hpp), the very list the world check applies.

/// Imports the host function and a WASI function, exports a memory and a function.
///
///     (module
///       (import "contract:test/host@0" "ping" (func (param i32)))
///       (import "wasi_snapshot_preview1" "fd_write" (func (param i32 i32 i32 i32) (result i32)))
///       (memory (export "memory") 1)
///       (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
inline constexpr auto DescribedModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x13, 0x03, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x04,
    0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x02, 0x3f, 0x02, 0x14, 0x63,
    0x6f, 0x6e, 0x74, 0x72, 0x61, 0x63, 0x74, 0x3a, 0x74, 0x65, 0x73, 0x74, 0x2f, 0x68, 0x6f, 0x73, 0x74,
    0x40, 0x30, 0x04, 0x70, 0x69, 0x6e, 0x67, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e,
    0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x08, 0x66,
    0x64, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x01, 0x03, 0x02, 0x01, 0x02, 0x05, 0x03, 0x01, 0x00,
    0x01, 0x07, 0x10, 0x02, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x03, 0x61, 0x64, 0x64,
    0x00, 0x02, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b,
});

/// Imports what no runtime provides.
///
///     (module (import "nowhere" "f" (func)))
inline constexpr auto UnknownImportModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x02,
    0x0d, 0x01, 0x07, 0x6e, 0x6f, 0x77, 0x68, 0x65, 0x72, 0x65, 0x01, 0x66, 0x00, 0x00,
});

/// Imports the host function with one parameter too many.
///
///     (module (import "contract:test/host@0" "ping" (func (param i32 i32))))
inline constexpr auto WrongSignatureModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x00,
    0x02, 0x1d, 0x01, 0x14, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x61, 0x63, 0x74, 0x3a, 0x74, 0x65, 0x73,
    0x74, 0x2f, 0x68, 0x6f, 0x73, 0x74, 0x40, 0x30, 0x04, 0x70, 0x69, 0x6e, 0x67, 0x00, 0x00,
});

/// Imports every allowed WASI preview 1 function, with its preview 1 signature.
///
///     (module
///       (import "wasi_snapshot_preview1" "args_get" (func (param i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "args_sizes_get" (func (param i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "environ_get" (func (param i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "environ_sizes_get" (func (param i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "clock_time_get" (func (param i32 i64 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "clock_res_get" (func (param i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "fd_write" (func (param i32 i32 i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "fd_close" (func (param i32) (result i32)))
///       (import "wasi_snapshot_preview1" "fd_fdstat_get" (func (param i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "fd_prestat_get" (func (param i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "fd_prestat_dir_name" (func (param i32 i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "fd_seek" (func (param i32 i64 i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "random_get" (func (param i32 i32) (result i32)))
///       (import "wasi_snapshot_preview1" "proc_exit" (func (param i32)))
///       (import "wasi_snapshot_preview1" "sched_yield" (func (result i32)))
///       (memory (export "memory") 1))
inline constexpr auto AllowedWasiModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x32, 0x08, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x03, 0x7f, 0x7e, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x01,
    0x7f, 0x01, 0x7f, 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7e, 0x7f, 0x7f, 0x01,
    0x7f, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x00, 0x01, 0x7f, 0x02, 0xb7, 0x04, 0x0f, 0x16, 0x77, 0x61, 0x73,
    0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65,
    0x77, 0x31, 0x08, 0x61, 0x72, 0x67, 0x73, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73,
    0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65,
    0x77, 0x31, 0x0e, 0x61, 0x72, 0x67, 0x73, 0x5f, 0x73, 0x69, 0x7a, 0x65, 0x73, 0x5f, 0x67, 0x65, 0x74,
    0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f,
    0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x0b, 0x65, 0x6e, 0x76, 0x69, 0x72, 0x6f, 0x6e, 0x5f,
    0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68,
    0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x11, 0x65, 0x6e, 0x76, 0x69, 0x72,
    0x6f, 0x6e, 0x5f, 0x73, 0x69, 0x7a, 0x65, 0x73, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61,
    0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31, 0x0e, 0x63, 0x6c, 0x6f, 0x63, 0x6b, 0x5f, 0x74, 0x69, 0x6d, 0x65, 0x5f, 0x67, 0x65,
    0x74, 0x00, 0x01, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74,
    0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x0d, 0x63, 0x6c, 0x6f, 0x63, 0x6b, 0x5f, 0x72,
    0x65, 0x73, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61,
    0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x08, 0x66, 0x64,
    0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x02, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61,
    0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x08, 0x66, 0x64,
    0x5f, 0x63, 0x6c, 0x6f, 0x73, 0x65, 0x00, 0x03, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61,
    0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x0d, 0x66, 0x64,
    0x5f, 0x66, 0x64, 0x73, 0x74, 0x61, 0x74, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73,
    0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65,
    0x77, 0x31, 0x0e, 0x66, 0x64, 0x5f, 0x70, 0x72, 0x65, 0x73, 0x74, 0x61, 0x74, 0x5f, 0x67, 0x65, 0x74,
    0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f,
    0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x13, 0x66, 0x64, 0x5f, 0x70, 0x72, 0x65, 0x73, 0x74,
    0x61, 0x74, 0x5f, 0x64, 0x69, 0x72, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x04, 0x16, 0x77, 0x61, 0x73,
    0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65,
    0x77, 0x31, 0x07, 0x66, 0x64, 0x5f, 0x73, 0x65, 0x65, 0x6b, 0x00, 0x05, 0x16, 0x77, 0x61, 0x73, 0x69,
    0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77,
    0x31, 0x0a, 0x72, 0x61, 0x6e, 0x64, 0x6f, 0x6d, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61,
    0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31, 0x09, 0x70, 0x72, 0x6f, 0x63, 0x5f, 0x65, 0x78, 0x69, 0x74, 0x00, 0x06, 0x16, 0x77,
    0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76,
    0x69, 0x65, 0x77, 0x31, 0x0b, 0x73, 0x63, 0x68, 0x65, 0x64, 0x5f, 0x79, 0x69, 0x65, 0x6c, 0x64, 0x00,
    0x07, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x0a, 0x01, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02,
    0x00,
});

/// What the instance cases run: calls `ping`, adds, stores, grows its memory, recurses, spins and
/// prints. `ping-then-store` calls `ping(42)`, then stores 1 at address 0; `grow` traps if its memory
/// refuses to grow by 4 pages; `print` writes "hello" (at address 16) to standard output.
///
///     (module
///       (import "contract:test/host@0" "ping" (func $ping (param i32)))
///       (import "wasi_snapshot_preview1" "fd_write" (func $fdWrite (param i32 i32 i32 i32) (result i32)))
///       (memory (export "memory") 1)
///       (data (i32.const 16) "hello")
///       (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1)))
///       (func (export "ping-then-store") (call $ping (i32.const 42)) (i32.store (i32.const 0) (i32.const
///       1))) (func (export "ping-then-add") (param i32 i32) (result i32)
///         (call $ping (i32.const 42)) (i32.add (local.get 0) (local.get 1)))
///       (func (export "grow") (if (i32.eq (memory.grow (i32.const 4)) (i32.const -1)) (then unreachable)))
///       (func $recurse (export "recurse") (call $recurse))
///       (func (export "spin") (loop (br 0)))
///       (func (export "print") (result i32)
///         (i32.store (i32.const 32) (i32.const 16))
///         (i32.store (i32.const 36) (i32.const 5))
///         (call $fdWrite (i32.const 1) (i32.const 32) (i32.const 1) (i32.const 40))))
inline constexpr auto InstanceModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x1a, 0x05, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x04,
    0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x00, 0x60, 0x00,
    0x01, 0x7f, 0x02, 0x3f, 0x02, 0x14, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x61, 0x63, 0x74, 0x3a, 0x74, 0x65,
    0x73, 0x74, 0x2f, 0x68, 0x6f, 0x73, 0x74, 0x40, 0x30, 0x04, 0x70, 0x69, 0x6e, 0x67, 0x00, 0x00, 0x16,
    0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65,
    0x76, 0x69, 0x65, 0x77, 0x31, 0x08, 0x66, 0x64, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x01, 0x03,
    0x08, 0x07, 0x02, 0x03, 0x02, 0x03, 0x03, 0x03, 0x04, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x52, 0x08,
    0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x03, 0x61, 0x64, 0x64, 0x00, 0x02, 0x0f, 0x70,
    0x69, 0x6e, 0x67, 0x2d, 0x74, 0x68, 0x65, 0x6e, 0x2d, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x00, 0x03, 0x0d,
    0x70, 0x69, 0x6e, 0x67, 0x2d, 0x74, 0x68, 0x65, 0x6e, 0x2d, 0x61, 0x64, 0x64, 0x00, 0x04, 0x04, 0x67,
    0x72, 0x6f, 0x77, 0x00, 0x05, 0x07, 0x72, 0x65, 0x63, 0x75, 0x72, 0x73, 0x65, 0x00, 0x06, 0x04, 0x73,
    0x70, 0x69, 0x6e, 0x00, 0x07, 0x05, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x00, 0x08, 0x0a, 0x59, 0x07, 0x07,
    0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b, 0x0d, 0x00, 0x41, 0x2a, 0x10, 0x00, 0x41, 0x00, 0x41, 0x01,
    0x36, 0x02, 0x00, 0x0b, 0x0b, 0x00, 0x41, 0x2a, 0x10, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b, 0x0d,
    0x00, 0x41, 0x04, 0x40, 0x00, 0x41, 0x7f, 0x46, 0x04, 0x40, 0x00, 0x0b, 0x0b, 0x04, 0x00, 0x10, 0x06,
    0x0b, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b, 0x1a, 0x00, 0x41, 0x20, 0x41, 0x10, 0x36, 0x02,
    0x00, 0x41, 0x24, 0x41, 0x05, 0x36, 0x02, 0x00, 0x41, 0x01, 0x41, 0x20, 0x41, 0x01, 0x41, 0x28, 0x10,
    0x01, 0x0b, 0x0b, 0x0b, 0x01, 0x00, 0x41, 0x10, 0x0b, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f,
});

/// Passes its two parameters on to the two-parameter host function, in order.
///
///     (module
///       (import "contract:test/host@0" "pair" (func $pair (param i32 i32)))
///       (memory (export "memory") 1)
///       (func (export "pair") (param i32 i32) (call $pair (local.get 0) (local.get 1))))
inline constexpr auto PairModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x02, 0x7f, 0x7f,
    0x00, 0x02, 0x1d, 0x01, 0x14, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x61, 0x63, 0x74, 0x3a, 0x74,
    0x65, 0x73, 0x74, 0x2f, 0x68, 0x6f, 0x73, 0x74, 0x40, 0x30, 0x04, 0x70, 0x61, 0x69, 0x72,
    0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x11, 0x02, 0x06,
    0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x04, 0x70, 0x61, 0x69, 0x72, 0x00, 0x01,
    0x0a, 0x0a, 0x01, 0x08, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x00, 0x0b,
});

/// The WebAssembly page size, in bytes.
inline constexpr auto PageBytes = uint32_t { 64 } * 1024;

/// One host function call, as a RecordingHostContext saw it.
struct HostCall
{
    std::size_t functionIndex = 0;   ///< The host function's index in the runtime's table.
    std::vector<uint32_t> arguments; ///< Its flat arguments.
    std::thread::id thread;          ///< The thread it ran on.

    /// @return Whether both calls are the same.
    bool operator==(HostCall const&) const = default;
};

/// A host context recording every host function call made through it, each answered as constructed.
class RecordingHostContext final: public IHostContext
{
  public:
    /// @param answer What every host function call returns.
    explicit RecordingHostContext(std::expected<void, ExtensionError> answer = {}) noexcept:
        _answer { std::move(answer) }
    {
    }

    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t functionIndex, std::span<uint32_t const> arguments) noexcept override
    {
        _calls.push_back(HostCall { .functionIndex = functionIndex,
                                    .arguments = { arguments.begin(), arguments.end() },
                                    .thread = std::this_thread::get_id() });
        return _answer;
    }

    /// @return The calls made so far, in order.
    [[nodiscard]] std::vector<HostCall> const& calls() const noexcept { return _calls; }

  private:
    std::expected<void, ExtensionError> _answer;
    std::vector<HostCall> _calls;
};

/// Runs @p body on a thread of its own and hands back what it returned. Catch2's assertions are not
/// thread-safe: the thread records, the test thread checks.
template <typename Body>
[[nodiscard]] auto onFreshThread(Body const& body)
{
    auto result = std::optional<decltype(body())> {};
    auto worker = std::jthread([&] { result.emplace(body()); });
    worker.join();
    return std::move(*result);
}

/// Runs @p body on a thread of its own, prepared by @p backend, and hands back what it returned.
/// @return What @p body returned; nothing if the thread could not be prepared.
template <typename Body>
[[nodiscard]] auto onPreparedThread(IWasmBackend& backend, Body const& body)
{
    return onFreshThread([&]() -> std::optional<decltype(body())> {
        auto const guard = backend.enterThread();
        if (!guard)
            return std::nullopt;
        return body();
    });
}

/// @return A runtime of @p backend, created with the contract's host functions and @p limits.
[[nodiscard]] inline std::shared_ptr<IWasmRuntime> createRuntime(IWasmBackend& backend,
                                                                 RuntimeLimits limits = Limits)
{
    auto runtime = backend.createRuntime(limits, HostFunctions);
    REQUIRE(runtime.has_value());
    return std::move(*runtime);
}

/// @return A guard preparing the calling thread, which @p backend must grant.
[[nodiscard]] inline std::unique_ptr<IThreadGuard> enterThread(IWasmBackend& backend)
{
    auto guard = backend.enterThread();
    REQUIRE(guard.has_value());
    return std::move(*guard);
}

/// @return @p module instantiated with @p context, which must succeed.
[[nodiscard]] inline std::unique_ptr<IWasmInstance> instantiate(IWasmModule& module, IHostContext& context)
{
    auto instance = module.instantiate(context);
    REQUIRE(instance.has_value());
    return std::move(*instance);
}

/// @return The function export of @p instance named @p name, which must be there.
[[nodiscard]] inline ExportFunction functionOf(IWasmInstance const& instance, std::string_view name)
{
    auto const function = instance.function(name);
    REQUIRE(function.has_value());
    return *function;
}

/// @return The 32-bit little-endian word at @p address of @p instance's memory, which must be there.
[[nodiscard]] inline uint32_t wordAt(IWasmInstance& instance, uint32_t address)
{
    auto const view = instance.memory(address, 4);
    REQUIRE(view.has_value());
    auto word = uint32_t { 0 };
    for (auto const [index, byte]: core::views::enumerate(*view))
        word |= std::to_integer<uint32_t>(byte) << (static_cast<uint32_t>(index) * 8);
    return word;
}

/// @return @p bytes loaded on @p runtime, which must accept them.
template <std::size_t N>
[[nodiscard]] std::shared_ptr<IWasmModule> load(IWasmRuntime& runtime, std::array<uint8_t, N> const& bytes)
{
    auto module = runtime.load(vtextension::testing::toModule(bytes));
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// @return The import of @p module named @p moduleName : @p name, which must be there.
[[nodiscard]] inline ImportDescription const& importOf(IWasmModule const& module,
                                                       std::string_view moduleName,
                                                       std::string_view name)
{
    auto const imports = module.imports();
    auto const entry = std::ranges::find_if(imports, [&](ImportDescription const& import) {
        return import.module == moduleName && import.name == name;
    });
    REQUIRE(entry != imports.end());
    return *entry;
}

/// @return The export of @p module named @p name, which must be there.
[[nodiscard]] inline ExportDescription const& exportOf(IWasmModule const& module, std::string_view name)
{
    auto const exports = module.exports();
    auto const entry = std::ranges::find(exports, name, &ExportDescription::name);
    REQUIRE(entry != exports.end());
    return *entry;
}

/// Enters a thread guard on the calling thread and checks that it prepares the thread.
/// @return Nothing if the thread is prepared while the guard is held; otherwise why not.
[[nodiscard]] inline std::expected<void, ExtensionError> enterAndCheck(IWasmBackend& backend)
{
    auto const guard = backend.enterThread();
    if (!guard)
        return std::unexpected(guard.error());
    if (!backend.isCurrentThreadPrepared())
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    return {};
}

/// Bytes that are no module are refused.
template <typename T>
void loadRejectsNonModule()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const loaded = runtime->load({ std::byte { 1 }, std::byte { 2 } });
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ExtensionError::InvalidComponent);
}

/// A module's imports and exports are described as declared, with what the runtime provides.
template <typename T>
void describesImportsAndExports()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const module = load(*runtime, DescribedModule);

    CHECK(module->imports().size() == 2);
    auto const& ping = importOf(*module, HostModule, "ping");
    CHECK(ping.kind == ExternKind::Function);
    CHECK(ping.signature == FunctionSignature { .parameters = { ValueType::I32 }, .results = {} });
    CHECK(ping.linkage == Linkage::Satisfied);
    auto const& fdWrite = importOf(*module, WasiModule, "fd_write");
    CHECK(fdWrite.kind == ExternKind::Function);
    CHECK(fdWrite.signature
          == FunctionSignature {
              .parameters = { ValueType::I32, ValueType::I32, ValueType::I32, ValueType::I32 },
              .results = { ValueType::I32 } });
    CHECK(fdWrite.linkage == Linkage::Satisfied);

    CHECK(module->exports().size() == 2);
    auto const& memory = exportOf(*module, "memory");
    CHECK(memory.kind == ExternKind::Memory);
    CHECK(memory.initialPages == 1);
    auto const& add = exportOf(*module, "add");
    CHECK(add.kind == ExternKind::Function);
    CHECK(add.signature
          == FunctionSignature { .parameters = { ValueType::I32, ValueType::I32 },
                                 .results = { ValueType::I32 } });
}

/// An import nothing provides is described as unsatisfied at load time; the load itself succeeds.
template <typename T>
void unknownImportIsUnsatisfied()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const module = load(*runtime, UnknownImportModule);
    CHECK(importOf(*module, "nowhere", "f").linkage == Linkage::Unsatisfied);
}

/// A host function imported with another signature than it is provided with is unsatisfied, rather
/// than linked and then trapping when called.
template <typename T>
void wrongSignatureImportIsUnsatisfied()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const module = load(*runtime, WrongSignatureModule);
    auto const& ping = importOf(*module, HostModule, "ping");
    CHECK(ping.signature
          == FunctionSignature { .parameters = { ValueType::I32, ValueType::I32 }, .results = {} });
    CHECK(ping.linkage == Linkage::Unsatisfied);
}

/// Every WASI preview 1 function the extension host allows is provided.
template <typename T>
void allowListedWasiIsSatisfied()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const module = load(*runtime, AllowedWasiModule);
    CHECK(module->imports().size() == AllowedWasiFunctions.size());
    for (auto const name: AllowedWasiFunctions)
    {
        INFO("WASI function " << name);
        CHECK(importOf(*module, WasiModule, name).linkage == Linkage::Satisfied);
    }
}

/// With no runtime alive there is nothing to prepare a thread for; with one, a guard prepares it.
template <typename T>
void threadGuardNeedsARuntime()
{
    auto const backend = T::make();
    auto const refused = onFreshThread([&] { return enterAndCheck(*backend); });
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::RuntimeUnavailable);

    auto const runtime = createRuntime(*backend);
    CHECK(onFreshThread([&] { return enterAndCheck(*backend); }).has_value());
}

/// Guards nest: ending an inner one leaves the thread prepared.
template <typename T>
void threadGuardNests()
{
    struct Report
    {
        bool outerEntered = false;
        bool innerEntered = false;
        bool preparedAfterInner = false;
    };

    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const report = onFreshThread([&] {
        auto result = Report {};
        auto const outer = backend->enterThread();
        result.outerEntered = outer.has_value();
        {
            auto const inner = backend->enterThread();
            result.innerEntered = inner.has_value();
        }
        result.preparedAfterInner = backend->isCurrentThreadPrepared();
        return result;
    });
    CHECK(report.outerEntered);
    CHECK(report.innerEntered);
    CHECK(report.preparedAfterInner);
}

/// A guard entered for one bring-up of the engine does not prepare its thread for the next.
template <typename T>
void generationFollowsBringUp()
{
    struct Report
    {
        std::optional<uint64_t> enteredGeneration;
        bool preparedAfterBringUp = true;
    };

    auto const backend = T::make();
    auto runtime = createRuntime(*backend);
    auto const firstGeneration = backend->runtimeGeneration();

    // The promise that resumes the worker is declared after the jthread, so that on any way out (a
    // throw included) it is destroyed first: that wakes the worker, as a broken promise, before the
    // jthread joins it, rather than leaving the join waiting for a bring-up that never comes.
    auto entered = std::promise<void> {};
    auto enteredFuture = entered.get_future();
    auto resumeLater = std::promise<void> {};
    auto report = Report {};
    auto worker = std::jthread([&, resume = resumeLater.get_future()] {
        auto const guard = backend->enterThread();
        if (guard)
            report.enteredGeneration = (*guard)->generation();
        entered.set_value();
        resume.wait();
        report.preparedAfterBringUp = backend->isCurrentThreadPrepared();
    });
    auto broughtUpAgain = std::move(resumeLater);
    enteredFuture.wait();
    runtime.reset();
    auto broughtUp = backend->createRuntime(Limits, HostFunctions);
    auto const secondGeneration = backend->runtimeGeneration();
    broughtUpAgain.set_value();
    worker.join();

    REQUIRE(broughtUp.has_value());
    CHECK(report.enteredGeneration == firstGeneration);
    CHECK(secondGeneration > firstGeneration);
    CHECK_FALSE(report.preparedAfterBringUp);
}

/// All backends of one kind behave as one.
template <typename T>
void twoBackendsAgree()
{
    struct Report
    {
        bool preparedViaOther = false;
        uint64_t generation = 0;
        uint64_t otherGeneration = 0;
    };

    auto const backend = T::make();
    auto const other = T::make();
    auto const runtime = createRuntime(*backend);
    auto const report = onFreshThread([&] {
        auto const guard = backend->enterThread();
        return Report { .preparedViaOther = guard.has_value() && other->isCurrentThreadPrepared(),
                        .generation = backend->runtimeGeneration(),
                        .otherGeneration = other->runtimeGeneration() };
    });
    CHECK(report.preparedViaOther);
    CHECK(report.generation == report.otherGeneration);
}

/// A module loaded on one prepared thread is used on another.
template <typename T>
void moduleCrossesThreads()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const loaded = onFreshThread([&]() -> std::expected<std::shared_ptr<IWasmModule>, ExtensionError> {
        auto const guard = backend->enterThread();
        if (!guard)
            return std::unexpected(guard.error());
        return runtime->load(vtextension::testing::toModule(DescribedModule));
    });
    REQUIRE(loaded.has_value());
    auto const& module = *loaded;
    CHECK(module->imports().size() == 2);
    CHECK(exportOf(*module, "add").kind == ExternKind::Function);
}

/// An instance's function export is called with its parameters and hands back its results.
template <typename T>
void callsExport()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);

    CHECK_FALSE(instance->function("nothing").has_value());
    CHECK_FALSE(instance->function("memory").has_value());
    auto argv = std::array<uint32_t, 2> { 2, 3 };
    REQUIRE(instance->call(functionOf(*instance, "add"), argv).has_value());
    CHECK(argv[0] == 5);
    CHECK(context.calls().empty());
}

/// A memory view lies inside the memory, or there is none.
template <typename T>
void memoryViewIsBoundsChecked()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);

    auto const whole = instance->memory(0, PageBytes);
    REQUIRE(whole.has_value());
    CHECK(whole->size() == PageBytes);
    auto const hello = instance->memory(16, 5);
    REQUIRE(hello.has_value());
    CHECK(std::ranges::equal(*hello, std::string_view { "hello" }, [](std::byte byte, char c) {
        return std::to_integer<char>(byte) == c;
    }));
    CHECK_FALSE(instance->memory(PageBytes - 1, 2).has_value());
    CHECK_FALSE(instance->memory(0xFFFFFFF0, 32).has_value());
    auto const empty = instance->memory(70000, 0);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

/// A host function call reaches the instance's context, with its index and arguments, synchronously
/// on the calling thread; the guest carries on after it.
template <typename T>
void hostCallbackReceivesArguments()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);

    CHECK(instance->call(functionOf(*instance, "ping-then-store"), {}).has_value());
    CHECK(context.calls()
          == std::vector {
              HostCall { .functionIndex = 0, .arguments = { 42 }, .thread = std::this_thread::get_id() } });
    CHECK(wordAt(*instance, 0) == 1);
}

/// A host function taking several parameters receives them in the order the guest passed them, under
/// its own index in the runtime's table.
template <typename T>
void hostCallbackReceivesArgumentsInOrder()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, PairModule);
    CHECK(importOf(*module, HostModule, "pair").linkage == Linkage::Satisfied);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);

    auto argv = std::array<uint32_t, 2> { 7, 9 };
    CHECK(instance->call(functionOf(*instance, "pair"), argv).has_value());
    CHECK(context.calls()
          == std::vector {
              HostCall { .functionIndex = 1, .arguments = { 7, 9 }, .thread = std::this_thread::get_id() } });
}

/// A host function that fails stops the guest: the call fails as a host failure, and nothing the
/// guest would have done after the host call happens.
template <typename T>
void hostCallbackErrorFails()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext { std::unexpected(ExtensionError::PermissionDenied) };
    auto const instance = instantiate(*module, context);

    auto const called = instance->call(functionOf(*instance, "ping-then-store"), {});
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error() == CallFailure::HostFailure);
    CHECK(context.calls().size() == 1);
    CHECK(wordAt(*instance, 0) == 0);
}

/// Which of two runtimes alive at once goes first.
enum class FirstToGo : uint8_t
{
    Older, ///< The one created first.
    Newer, ///< The one created second.
};

/// Two runtimes with the same host functions are alive at once, and @p firstToGo goes: what was loaded
/// on the other keeps linking and calling.
template <typename T>
void survivorKeepsHostImports(FirstToGo firstToGo)
{
    auto const backend = T::make();
    auto older = createRuntime(*backend);
    auto newer = createRuntime(*backend);
    auto& gone = firstToGo == FirstToGo::Older ? older : newer;
    auto const& survivor = firstToGo == FirstToGo::Older ? newer : older;
    auto const guard = enterThread(*backend);
    auto const module = load(*survivor, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);

    gone.reset();

    CHECK(instance->call(functionOf(*instance, "ping-then-store"), {}).has_value());
    REQUIRE(context.calls().size() == 1);
    CHECK(context.calls().front().functionIndex == 0);
    CHECK(context.calls().front().arguments == std::vector<uint32_t> { 42 });
    auto const later = load(*survivor, InstanceModule);
    CHECK(importOf(*later, HostModule, "ping").linkage == Linkage::Satisfied);
}

/// Two runtimes with the same host functions are alive at once, and the older one goes: the newer
/// one's host imports keep linking and calling.
template <typename T>
void twoRuntimesShareHostImports()
{
    survivorKeepsHostImports<T>(FirstToGo::Older);
}

/// Two runtimes with the same host functions are alive at once, and the newer one goes: what was
/// loaded on the older one keeps linking and calling.
template <typename T>
void olderRuntimeOutlivesNewer()
{
    survivorKeepsHostImports<T>(FirstToGo::Newer);
}

/// Memory refuses to grow past the runtime's cap, and the guest's trap fails the call.
template <typename T>
void memoryCapRefusesGrowth()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(
        *backend,
        RuntimeLimits { .memoryBytes = std::size_t { 2 } * PageBytes, .stackBytes = Limits.stackBytes });
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);

    auto const called = instance->call(functionOf(*instance, "grow"), {});
    REQUIRE_FALSE(called.has_value());
    auto const expected = backend->capabilities().memoryGrowth == MemoryGrowthReporting::Reported
                              ? CallFailure::MemoryGrowthRefused
                              : CallFailure::Trap;
    CHECK(called.error() == expected);
    CHECK_FALSE(instance->memory(0, 2 * PageBytes).has_value());
}

/// A memory whose initial size exceeds the runtime's cap is not instantiated.
template <typename T>
void memoryCapRefusesInitialSize()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(
        *backend, RuntimeLimits { .memoryBytes = PageBytes / 2, .stackBytes = Limits.stackBytes });
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = module->instantiate(context);
    REQUIRE_FALSE(instance.has_value());
    CHECK(instance.error() == ExtensionError::OutOfMemory);
}

/// Unbounded recursion runs into the stack cap and traps.
template <typename T>
void unboundedRecursionTraps()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);

    auto const called = instance->call(functionOf(*instance, "recurse"), {});
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error() == CallFailure::Trap);
}

/// An interrupt from another thread stops a call that would never return.
template <typename T>
void interruptStopsLoop()
{
    using namespace std::chrono_literals;

    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);
    auto const spin = functionOf(*instance, "spin");

    auto const started = std::chrono::steady_clock::now();
    auto interrupter = std::jthread([&] {
        std::this_thread::sleep_for(50ms);
        instance->interrupt();
    });
    auto const called = instance->call(spin, {});
    auto const elapsed = std::chrono::steady_clock::now() - started;
    interrupter.join();

    REQUIRE_FALSE(called.has_value());
    CHECK(called.error() == CallFailure::Interrupted);
    CHECK(elapsed < 5s);
}

/// An interrupt with no call running is final: every later call fails without entering the guest.
template <typename T>
void interruptIsFinal()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);
    auto const pingThenAdd = functionOf(*instance, "ping-then-add");

    instance->interrupt();

    for (auto const attempt: std::views::iota(0, 2))
    {
        INFO("attempt " << attempt);
        auto argv = std::array<uint32_t, 2> { 2, 3 };
        auto const called = instance->call(pingThenAdd, argv);
        REQUIRE_FALSE(called.has_value());
        CHECK(called.error() == CallFailure::Interrupted);
    }
    CHECK(context.calls().empty());
}

/// A module loaded on one prepared thread is instantiated on a second, and called on a third.
template <typename T>
void instanceCrossesThreads()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const loaded = onPreparedThread(
        *backend, [&] { return runtime->load(vtextension::testing::toModule(DescribedModule)); });
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    auto const& module = **loaded;

    auto context = RecordingHostContext {};
    auto created = onPreparedThread(*backend, [&] { return module->instantiate(context); });
    REQUIRE(created.has_value());
    REQUIRE(created->has_value());
    auto const instance = std::move(**created);

    auto const sum = onPreparedThread(*backend, [&]() -> std::expected<uint32_t, CallFailure> {
        auto const add = instance->function("add");
        if (!add)
            return std::unexpected(CallFailure::Trap);
        auto argv = std::array<uint32_t, 2> { 2, 3 };
        return instance->call(*add, argv).transform([&] { return argv[0]; });
    });
    REQUIRE(sum.has_value());
    CHECK(*sum == std::expected<uint32_t, CallFailure> { 5 });
}

#ifndef _WIN32
/// The signals whose disposition a backend must leave alone.
inline constexpr auto GuardedSignals = std::array { SIGSEGV, SIGUSR1 };

/// @return The disposition of @p signal.
[[nodiscard]] inline struct sigaction dispositionOf(int signal)
{
    struct sigaction action {};
    REQUIRE(sigaction(signal, nullptr, &action) == 0);
    return action;
}

/// Checks that @p after is the disposition @p before was.
inline void checkSameDisposition(struct sigaction const& before, struct sigaction const& after)
{
    CHECK(after.sa_sigaction == before.sa_sigaction);
    CHECK(after.sa_flags == before.sa_flags);
}

/// No trap handler for out-of-bounds accesses, no wake-up signal: bringing the engine up, and preparing
/// a thread to run guests, leaves the process's (or a sanitizer's) handlers in charge.
template <typename T>
void installsNoSignalHandlers()
{
    for (auto const signal: GuardedSignals)
    {
        INFO("signal " << signal);
        auto const before = dispositionOf(signal);
        auto const backend = T::make();
        auto const generationBefore = backend->runtimeGeneration();
        auto const runtime = createRuntime(*backend);
        auto const afterCreate = dispositionOf(signal);
        // The engine was brought up here, rather than joined: what is checked is a bring-up.
        CHECK(backend->runtimeGeneration() > generationBefore);
        auto const prepared = onFreshThread([&] { return enterAndCheck(*backend); });
        auto const afterEnter = dispositionOf(signal);
        CHECK(prepared.has_value());
        checkSameDisposition(before, afterCreate);
        checkSameDisposition(before, afterEnter);
    }
}

/// What a guest writes to its standard output reaches nobody, and the write succeeds.
template <typename T>
void wasiStdioGoesNowhere()
{
    auto const backend = T::make();
    auto const runtime = createRuntime(*backend);
    auto const guard = enterThread(*backend);
    auto const module = load(*runtime, InstanceModule);
    auto context = RecordingHostContext {};
    auto const instance = instantiate(*module, context);
    auto const print = functionOf(*instance, "print");

    auto capture = vtextension::testing::StandardStreamsCapture {};
    auto argv = std::array<uint32_t, 1> {};
    auto const called = instance->call(print, argv);
    auto const captured = capture.finish();

    CHECK(called.has_value());
    CHECK(argv[0] == 0); // fd_write's errno: the write succeeded.
    INFO("captured: " << captured);
    CHECK(captured.empty());
}

    // A macro cannot hold a preprocessor conditional, so the POSIX-only cases have a macro of their own.
    #define CONTOUR_WASM_BACKEND_CONTRACT_POSIX(Traits)                      \
        CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, installsNoSignalHandlers) \
        CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, wasiStdioGoesNowhere)
#else
    #define CONTOUR_WASM_BACKEND_CONTRACT_POSIX(Traits)
#endif

} // namespace vtextension::wasm::testing::contract

/// One contract case, run against the backend of @p Traits.
#define CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, Name)                 \
    TEMPLATE_TEST_CASE("WasmContract." #Name, "[wasm-contract]", Traits) \
    {                                                                    \
        ::vtextension::wasm::testing::contract::Name<TestType>();        \
    }

/// Instantiates the whole contract suite against the backend @p Traits::make() creates.
#define CONTOUR_WASM_BACKEND_CONTRACT(Traits)                                        \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, loadRejectsNonModule)                 \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, describesImportsAndExports)           \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, unknownImportIsUnsatisfied)           \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, wrongSignatureImportIsUnsatisfied)    \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, allowListedWasiIsSatisfied)           \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, threadGuardNeedsARuntime)             \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, threadGuardNests)                     \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, generationFollowsBringUp)             \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, twoBackendsAgree)                     \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, moduleCrossesThreads)                 \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, callsExport)                          \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, memoryViewIsBoundsChecked)            \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, hostCallbackReceivesArguments)        \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, hostCallbackReceivesArgumentsInOrder) \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, hostCallbackErrorFails)               \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, twoRuntimesShareHostImports)          \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, olderRuntimeOutlivesNewer)            \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, memoryCapRefusesGrowth)               \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, memoryCapRefusesInitialSize)          \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, unboundedRecursionTraps)              \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, interruptStopsLoop)                   \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, interruptIsFinal)                     \
    CONTOUR_WASM_BACKEND_CONTRACT_CASE(Traits, instanceCrossesThreads)               \
    CONTOUR_WASM_BACKEND_CONTRACT_POSIX(Traits)
