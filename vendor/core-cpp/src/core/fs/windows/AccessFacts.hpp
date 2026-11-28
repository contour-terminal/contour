// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Windows reader of who may reach an object: the owner and DACL of an open handle, as
/// @c AccessFacts. Private to the fs module.

#include <core/fs/AccessScope.hpp>
#include <core/fs/FsExport.hpp>

#include <optional>

// As NtFile.hpp includes it: <ntstatus.h> defines every STATUS_ code, which <windows.h> defines some
// of again unless told not to, so a source that includes this header before NtFile.hpp would see
// them defined twice (MSVC's C4005).
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

namespace core::fs::detail
{

/// Reads the owner and the DACL of @p handle (`GetSecurityInfo`), every entry of it, inherited ones
/// included. An ALLOW or DENY entry is judged by the principal it names: @p user, LocalSystem,
/// BUILTIN\Administrators, or anyone else; any other entry type is `Unknown`. An inherit-only entry
/// (`INHERIT_ONLY_ACE`) is skipped: it grants nothing on this object, only on what is created
/// inside it, which carries the entry, and is judged, itself.
/// @param handle An open object, with `READ_CONTROL`.
/// @param user The process token's user.
/// @return The facts; nullopt where the handle's security cannot be read.
[[nodiscard]] CORE_FS_API std::optional<AccessFacts> accessFactsOf(HANDLE handle, PSID user);

} // namespace core::fs::detail
