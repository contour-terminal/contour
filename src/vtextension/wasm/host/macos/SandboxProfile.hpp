// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's sandbox profile on macOS, in Seatbelt's profile language (SBPL), and the parameters
/// it reads. Std-only, so that every system's tests read the text the host applies on macOS.
///
/// Deny by default; what is allowed is what the locked-down host still does that Seatbelt mediates:
/// - mapping the guest memory objects Contour created and granted it, and asking their size, by the prefix
///   their names carry (SharedObjectName.hpp): their data, and their metadata (fstat(), which XNU checks as
///   such). Never creating, truncating or unlinking one: Contour makes every object.
/// - signalling itself, as a runtime that raises a signal does;
/// - reading the page size (hw.pagesize_compat), which the C library's getpagesize() asks the kernel for on
///   every call until one succeeds: the interpreting engine asks as it maps an instance's memory, after the
///   lockdown, and without the answer maps none (the first macOS run's deny log, one denial per instance).
/// - reading the CPU feature sysctls (hw.optional.), kept defensively: the code generator reads them as it
///   creates its first engine, which priming a thread does before the lockdown, and caches what it read. A
///   read after the lockdown would only come from a library that does not cache; remove the rule once a
///   green macOS run shows its deny log never asks for it.
///
/// Nothing else: no network, no Mach service lookup, no file read or write, no new process, no exec.

#include <array>
#include <cstddef>
#include <string_view>

#include <vtextension/wasm/remote/transport/SharedObjectName.hpp>

namespace vtextension::wasm::host
{

/// A parameter the profile reads as `(param "<name>")`: C strings, as the C API takes them.
struct ProfileParameter
{
    char const* name;  ///< Its name.
    char const* value; ///< Its value.
};

/// Every parameter the profile reads.
inline constexpr auto ProfileParameters = std::array {
    ProfileParameter { .name = "SHM_PREFIX", .value = remote::transport::SharedObjectNamePrefix },
};

/// The profile: deny by default, and only what the locked-down host does.
inline constexpr auto SandboxProfile = std::string_view { R"sbpl((version 1)
(deny default)

; Guest memory: objects Contour created and granted, mapped (data) and fstat()ed (metadata). Never created.
(allow ipc-posix-shm-read-data ipc-posix-shm-read-metadata ipc-posix-shm-write-data
    (ipc-posix-name-prefix (param "SHM_PREFIX")))

; A runtime that raises a signal raises it on itself.
(allow signal (target self))

; The page size, which getpagesize() asks for until it is answered: the interpreter asks as it maps an
; instance's memory. And the CPU features the code generator reads: read and cached before the lockdown;
; kept in case a library reads them again later. To be removed once a green run shows no denial needs it.
(allow sysctl-read
    (sysctl-name "hw.pagesize_compat")
    (sysctl-name-prefix "hw.optional."))
)sbpl" };

/// @return ProfileParameters as sandbox_init_with_parameters() takes them: name, value, name, value, …,
///         then a null pointer.
[[nodiscard]] constexpr auto profileParameterList() noexcept
{
    auto list = std::array<char const*, (2 * ProfileParameters.size()) + 1> {};
    auto next = std::size_t { 0 };
    for (auto const& parameter: ProfileParameters)
    {
        list[next++] = parameter.name;
        list[next++] = parameter.value;
    }
    list[next] = nullptr;
    return list;
}

} // namespace vtextension::wasm::host
