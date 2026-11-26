// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Windows only: what an AppContainer the extension host is started in needs of its directory. A container
/// reads only what grants `ALL APPLICATION PACKAGES` (S-1-15-2-1) or its own SID, so the helper's directory
/// must grant the first read and execute access before the helper can start there.
///
/// The system is asked through an API table (AppContainerApi), so that what it answers can be faked in a
/// test.

#include <expected>
#include <filesystem>
#include <functional>
#include <string_view>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::process
{

/// The SID of `ALL APPLICATION PACKAGES`, which every AppContainer's token carries.
inline constexpr auto AllApplicationPackagesSid = std::wstring_view { L"S-1-15-2-1" };

/// The advapi functions a directory's grant is ensured through, one per thing the system is asked.
struct AppContainerApi
{
    /// GetNamedSecurityInfoW() of a file's DACL: the list in @p dacl, owned by @p descriptor (LocalFree()).
    std::function<DWORD(PCWSTR path, PACL* dacl, PSECURITY_DESCRIPTOR* descriptor)> readDacl;
    /// SetNamedSecurityInfoW() of a file's DACL, which propagates its inheritable entries to the children.
    std::function<DWORD(PCWSTR path, PACL dacl)> writeDacl;
};

/// @return The system's functions.
[[nodiscard]] AppContainerApi systemAppContainerApi();

/// Lets every AppContainer read and execute @p directory and the files in it, unless its DACL lets them
/// already -- whether by an entry of its own or an inherited one, as `Program Files` does. What it adds
/// reaches the files directly in @p directory, and nothing beneath it.
/// @param directory The helper's directory.
/// @param api What the system is asked through.
/// @return Nothing once they may; `Unsupported` if this user may not change the DACL (an installation it does
///         not own, whose installer must grant it), `Io` if the system refused otherwise.
[[nodiscard]] std::expected<void, LaunchError> ensurePackagesCanExecute(
    std::filesystem::path const& directory, AppContainerApi const& api);

} // namespace vtextension::wasm::remote::process
