// SPDX-License-Identifier: Apache-2.0
//
// Internal to the WAMR backend: the rewrite of a module's bytes that makes WAMR hold every table to the
// runtime's cap (RuntimeLimits::tableElements).
//
// WAMR has no option for a table cap. It lets a table grow to its declared maximum, but caps one declared
// without a maximum, or with a larger one, at twice its initial size or MaxTableElements, whichever is
// larger -- not at a cap of the host's choosing. So before WAMR sees a module, every table it defines is
// given a declared maximum no larger than the cap, and `table.grow` past the cap returns -1 as it does on
// every backend. A table whose initial size already exceeds the cap is left as it is: the backend refuses
// to instantiate it before WAMR could.
//
// Pure and dependency-free, so that every framing rule is table-testable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <vtextension/wasm/WasmBinary.hpp>

namespace vtextension::wasm::wamr
{

/// Gives every table @p module defines a declared maximum of at most @p maxElements, keeping a lower one.
/// Only the table section is rewritten, and only if a table needs it; every other section is copied as it
/// was.
/// @param module A WebAssembly core module's bytes.
/// @param tables Its tables, as binary::tableDeclarations() read them: the module is not read for them
///               again.
/// @param maxElements The cap on each table's elements.
/// @return The module, rewritten or as it was; why its sections could not be read.
[[nodiscard]] std::expected<std::vector<std::byte>, binary::BinaryError> capTableMaxima(
    std::vector<std::byte> module, std::span<binary::TableDeclaration const> tables, uint32_t maxElements);

} // namespace vtextension::wasm::wamr
