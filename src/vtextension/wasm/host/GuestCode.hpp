// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What an engine makes of a module's code: what decides whether the host it runs in may still map
/// executable memory once it is locked down.

#include <cstdint>

namespace vtextension::wasm::host
{

/// How an engine runs a module's code.
enum class GuestCode : uint8_t
{
    Interpreted, ///< Interpreted: once locked down, the host never makes memory executable.
    Compiled,    ///< Compiled to native code as it is loaded: written, then made executable.
};

} // namespace vtextension::wasm::host
