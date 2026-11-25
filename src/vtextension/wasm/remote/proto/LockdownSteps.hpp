// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one table that phrases every step of the extension host's confinement: the host tells of a step that
/// failed in these words, and Contour names the step a refused host's Hello states in them too.

#include <span>
#include <string_view>

#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::remote::proto
{

/// One step of the confinement, as users read of it.
struct LockdownStepRow
{
    LockdownStep step;     ///< The step.
    std::string_view text; ///< What failed if it fails, user-readable, naming no engine: a sentence's end.
};

/// @return Every LockdownStep, one row each, at its enumerator's index.
[[nodiscard]] std::span<LockdownStepRow const> lockdownSteps() noexcept;

/// @param step A step, or any value of its type.
/// @return What failed if @p step fails, as its row says; the row of `None` for a value naming no step.
[[nodiscard]] std::string_view lockdownStepText(LockdownStep step) noexcept;

} // namespace vtextension::wasm::remote::proto
