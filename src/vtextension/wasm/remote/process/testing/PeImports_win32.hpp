// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only, Windows only: what a PE image imports as it starts, read from its file, and the libraries the
/// loader resolves for an executable, so that a test can say which of them a process may load.

#include <filesystem>
#include <string>
#include <vector>

namespace vtextension::wasm::remote::process::testing
{

/// @return The libraries the executable or library @p file imports as it starts, lower-cased: its import
///         directory's, not those it delay-loads. Fails the case if @p file is no PE32+ image.
[[nodiscard]] std::vector<std::string> startupImportsOf(std::filesystem::path const& file);

/// @return The names of the system's known DLLs, lower-cased, as its registry lists them
///         (`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs`); none where it lists none.
[[nodiscard]] std::vector<std::string> knownDlls();

/// The libraries an executable loads as it starts, as far as their files say.
struct ImportClosure
{
    /// Each library found, every one after those it imports: the order the loader initialises them in. A file
    /// by its path; an API set by its bare name, which LoadLibraryExW() resolves through the system's API set
    /// schema to the library behind it (`api-ms-win-crt-*` to ucrtbase.dll), loading that one in its turn.
    std::vector<std::filesystem::path> libraries;
    /// Each import found neither beside the executable nor in the system directory.
    std::vector<std::string> unresolved;
};

/// @return What @p executable loads as it starts: its imports and theirs, each looked up as the loader
///         looks for one not already loaded -- a known DLL (the system's `KnownDLLs` list) in the system
///         directory, any other beside @p executable, then in the system directory. API sets (`api-ms-win-*`,
///         `ext-ms-win-*`) are no files: each is listed by its name, where it is first imported, and what it
///         maps onto is not read further.
[[nodiscard]] ImportClosure importClosureOf(std::filesystem::path const& executable);

} // namespace vtextension::wasm::remote::process::testing
