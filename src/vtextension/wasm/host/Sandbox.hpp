// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's confinement, as the host process phrases a step of it that failed.

#include <string>

#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::host
{

/// A step of the host's confinement that failed, and how.
struct ConfinementFailure
{
    remote::proto::LockdownStep step; ///< The step; never `None`.
    /// What the system said of it: errno on POSIX, an HRESULT on Windows.
    int code;

    bool operator==(ConfinementFailure const&) const = default;
};

/// @param failure A step that failed.
/// @return What failed and why, user-readable: the step's text (lockdownSteps()), then the system's word on
///         the error.
[[nodiscard]] std::string confinementFailureText(ConfinementFailure const& failure);

} // namespace vtextension::wasm::host
