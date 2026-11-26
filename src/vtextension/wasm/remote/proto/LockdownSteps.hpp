// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one table that phrases every step of the extension host's confinement: the host tells of a step that
/// failed in these words, and Contour names the step a refused host's Hello states in them too.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::remote::proto
{

/// What the code a step states when it fails is.
enum class FailureCodeKind : uint8_t
{
    System,    ///< What the system said: errno on POSIX, an HRESULT on Windows; 0 for nothing said.
    Extension, ///< An ExtensionError: what the extension runtime said.
};

/// One step of the confinement, as users read of it.
struct LockdownStepRow
{
    LockdownStep step;     ///< The step.
    std::string_view text; ///< What failed if it fails, user-readable, naming no engine: a sentence's end.
    FailureCodeKind code = FailureCodeKind::System; ///< What its code is.
};

/// A step that failed, and its code, as a Hello states them.
struct LockdownFailure
{
    LockdownStep step; ///< The step.
    uint32_t code;     ///< Its code, as its row's FailureCodeKind says; an HRESULT as its bit pattern.

    bool operator==(LockdownFailure const&) const = default;
};

/// @return Every LockdownStep, one row each, at its enumerator's index.
[[nodiscard]] std::span<LockdownStepRow const> lockdownSteps() noexcept;

/// @param step A step, or any value of its type.
/// @return What failed if @p step fails, as its row says; the row of `None` for a value naming no step.
[[nodiscard]] std::string_view lockdownStepText(LockdownStep step) noexcept;

/// @param code An error code a confinement step returned: errno on POSIX, an HRESULT on Windows.
/// @return What this system says of it, user-readable. Both ends of a build run on one system, so the parent
///         phrases the code a host states as the host would.
[[nodiscard]] std::string systemErrorText(int code);

/// @param failure A step that failed, and its code.
/// @return What its code says, as the step's row reads it, naming no engine; empty where it says nothing (a
///         system code of 0, or an ExtensionError with no words of its own here).
[[nodiscard]] std::string failureCodeText(LockdownFailure failure);

/// @param failure A step that failed, and its code.
/// @return What failed, user-readable: the step's text, then what its code says in parentheses where it says
///         anything (failureCodeText()).
[[nodiscard]] std::string lockdownFailureText(LockdownFailure failure);

} // namespace vtextension::wasm::remote::proto
