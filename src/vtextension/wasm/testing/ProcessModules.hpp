// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Modules for the cases that run an extension host process: hand-encoded, like the contract's.

#include <array>
#include <cstdint>

namespace vtextension::wasm::testing
{

// clang-format off
/// A start function that never returns: instantiating it hangs the host until it is stopped.
///
///     (module (func $s (loop (br 0))) (start $s))
inline constexpr auto StartSpinModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic, version 1
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,             // type section: () -> ()
    0x03, 0x02, 0x01, 0x00,                         // function section: $s has type 0
    0x08, 0x01, 0x00,                               // start section: $s
    0x0a, 0x09, 0x01, 0x07, 0x00,                   // code section: one body of 7 bytes, no locals
    0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b,             // loop, br 0, end; end
});

/// One memory of 1 page, growing to at most 16; `grow` grows it by its argument's pages and returns the old
/// page count (-1 if refused), `store` stores an i32 at an address.
///
///     (module
///       (memory (export "memory") 1 16)
///       (func (export "grow") (param i32) (result i32) (memory.grow (local.get 0)))
///       (func (export "store") (param i32 i32) (i32.store (local.get 0) (local.get 1))))
inline constexpr auto GrowModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic, version 1
    0x01, 0x0b, 0x02,                               // type section: two types
    0x60, 0x01, 0x7f, 0x01, 0x7f,                   //   (i32) -> (i32)
    0x60, 0x02, 0x7f, 0x7f, 0x00,                   //   (i32, i32) -> ()
    0x03, 0x03, 0x02, 0x00, 0x01,                   // function section: grow, store
    0x05, 0x04, 0x01, 0x01, 0x01, 0x10,             // memory section: min 1, max 16
    0x07, 0x19, 0x03,                               // export section: three exports
    0x06, 'm', 'e', 'm', 'o', 'r', 'y', 0x02, 0x00, //   memory 0
    0x04, 'g', 'r', 'o', 'w', 0x00, 0x00,           //   function 0
    0x05, 's', 't', 'o', 'r', 'e', 0x00, 0x01,      //   function 1
    0x0a, 0x12, 0x02,                               // code section: two bodies
    0x06, 0x00, 0x20, 0x00, 0x40, 0x00, 0x0b,       //   local.get 0, memory.grow; end
    0x09, 0x00, 0x20, 0x00, 0x20, 0x01,             //   local.get 0, local.get 1,
    0x36, 0x02, 0x00, 0x0b,                         //   i32.store; end
});

/// The address StartGrowModule's start function stores to, in the page it grew: 64 KiB + 8.
inline constexpr auto StartGrowAddress = uint32_t { 0x1'0008 };

/// What StartGrowModule's start function stores there.
inline constexpr auto StartGrowValue = uint32_t { 42 };

/// One memory of 1 page, growing to at most 16, whose start function grows it by a page, keeps what
/// memory.grow returned in a global (-1 if refused) and, if it grew, stores StartGrowValue at
/// StartGrowAddress; `grown` returns the global.
///
///     (module
///       (memory (export "memory") 1 16)
///       (global $grown (mut i32) (i32.const 0))
///       (func $start
///         (global.set $grown (memory.grow (i32.const 1)))
///         (if (i32.ne (global.get $grown) (i32.const -1))
///           (then (i32.store (i32.const 65544) (i32.const 42)))))
///       (func (export "grown") (result i32) (global.get $grown))
///       (start $start))
inline constexpr auto StartGrowModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic, version 1
    0x01, 0x08, 0x02,                               // type section: two types
    0x60, 0x00, 0x00,                               //   () -> ()
    0x60, 0x00, 0x01, 0x7f,                         //   () -> (i32)
    0x03, 0x03, 0x02, 0x00, 0x01,                   // function section: $start, grown
    0x05, 0x04, 0x01, 0x01, 0x01, 0x10,             // memory section: min 1, max 16
    0x06, 0x06, 0x01, 0x7f, 0x01, 0x41, 0x00, 0x0b, // global section: mut i32 = 0
    0x07, 0x12, 0x02,                               // export section: two exports
    0x06, 'm', 'e', 'm', 'o', 'r', 'y', 0x02, 0x00, //   memory 0
    0x05, 'g', 'r', 'o', 'w', 'n', 0x00, 0x01,      //   function 1
    0x08, 0x01, 0x00,                               // start section: $start
    0x0a, 0x20, 0x02,                               // code section: two bodies
    0x19, 0x00,                                     //   $start: 25 bytes, no locals
    0x41, 0x01, 0x40, 0x00, 0x24, 0x00,             //   i32.const 1, memory.grow, global.set 0
    0x23, 0x00, 0x41, 0x7f, 0x47,                   //   global.get 0, i32.const -1, i32.ne
    0x04, 0x40,                                     //   if
    0x41, 0x88, 0x80, 0x04, 0x41, 0x2a,             //     i32.const 65544, i32.const 42
    0x36, 0x02, 0x00,                               //     i32.store
    0x0b, 0x0b,                                     //   end; end
    0x04, 0x00, 0x23, 0x00, 0x0b,                   //   grown: global.get 0; end
});
// clang-format on

} // namespace vtextension::wasm::testing
