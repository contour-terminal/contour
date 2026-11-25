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
// clang-format on

} // namespace vtextension::wasm::testing
