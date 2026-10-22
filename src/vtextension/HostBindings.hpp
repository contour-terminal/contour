// SPDX-License-Identifier: Apache-2.0
//
// Internal to vtextension: the statusline world's host functions as the runtime links them.
#pragma once

#include <memory>

namespace vtextension::detail
{

/// The import module name the world's host functions live under: `<package>/<interface>@<version>`.
inline constexpr char const* HostImportModule = "terminal-extensions:statusline/host@0.1.0";

/// Keeps the host functions registered with the runtime for as long as it lives, and the callback
/// that tells an instance its memory could not grow. Each live Engine holds one; the runtime looks
/// imports up in every registration, so any one suffices.
class HostRegistration
{
  public:
    HostRegistration() = default;
    HostRegistration(HostRegistration const&) = delete;
    HostRegistration(HostRegistration&&) = delete;
    HostRegistration& operator=(HostRegistration const&) = delete;
    HostRegistration& operator=(HostRegistration&&) = delete;
    virtual ~HostRegistration() = default;
};

/// Registers the host functions with the running runtime.
/// @return The registration; null if the runtime refused it.
[[nodiscard]] std::unique_ptr<HostRegistration> registerHostFunctions();

} // namespace vtextension::detail
