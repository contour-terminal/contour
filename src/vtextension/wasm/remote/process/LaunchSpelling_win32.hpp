// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Windows only: how a launch is spelled for CreateProcessW() -- its command line and its environment block,
/// in UTF-16 -- and the variables a launch into an AppContainer cannot go without. Pure, so that what the
/// system parses is tested apart from starting anything.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtextension::wasm::remote::process
{

/// One environment variable: its name and its value, in UTF-8.
using EnvironmentEntry = std::pair<std::string, std::string>;

/// What a contained launch hands CreateProcessW() for one of ContainerVariables.
enum class ContainerVariableSource : uint8_t
{
    /// The folder the child is to see, beneath the container's own.
    ContainerFolder,
    /// The profile's local folder (`...\AppData\Local`) the container's lies in: CreateProcessW() extends
    /// what it is given to `<given>\Packages\<name>\AC`, which a GitHub Windows runner showed by doubling a
    /// container folder handed to it.
    ProfileFolder,
};

/// One variable a contained process is started with that names a folder of its AppContainer's own.
struct ContainerVariable
{
    std::string_view name;          ///< The variable.
    std::wstring_view subfolder;    ///< What the child sees: beneath the container's folder; empty for it.
    ContainerVariableSource source; ///< What CreateProcessW() is handed for it.
};

/// What a contained process's environment says of where it keeps its data, as Microsoft's "Launch an
/// AppContainer" spells it: LOCALAPPDATA the container's folder (`...\Packages\<name>\AC`), TEMP and TMP its
/// `Temp` beneath. CreateProcessW() refuses a contained launch whose block lacks them, with
/// ERROR_ENVVAR_NOT_FOUND (203), and derives each from the LOCALAPPDATA it is given, as a GitHub Windows
/// runner showed: so LOCALAPPDATA is handed the profile's folder, and TEMP and TMP what the child is to see.
inline constexpr auto ContainerVariables = std::to_array<ContainerVariable>({
    { .name = "LOCALAPPDATA", .subfolder = L"", .source = ContainerVariableSource::ProfileFolder },
    { .name = "TEMP", .subfolder = L"Temp", .source = ContainerVariableSource::ContainerFolder },
    { .name = "TMP", .subfolder = L"Temp", .source = ContainerVariableSource::ContainerFolder },
});

/// An AppContainer's own folder, and the profile's local folder it lies in.
struct ContainerFolders
{
    std::filesystem::path
        container;                 ///< Its own (GetAppContainerFolderPath()): `<profile>\Packages\<name>\AC`.
    std::filesystem::path profile; ///< The profile's local folder, `<profile>`.

    bool operator==(ContainerFolders const&) const = default;
};

/// @return @p containerFolder and the profile's local folder it lies in; nothing unless it reads
///         `<profile>\Packages\<name>\AC` (names compared ignoring ASCII case) with `<profile>` absolute,
///         which is how CreateProcessW() makes it from the profile's.
[[nodiscard]] std::optional<ContainerFolders> containerFoldersOf(
    std::filesystem::path const& containerFolder);

/// @return @p text, UTF-8, as UTF-16; nothing if it is not UTF-8.
[[nodiscard]] std::optional<std::wstring> wideOf(std::string_view text);

/// Appends @p argument to @p commandLine, quoted as the C runtime's argv parser reads it back.
void appendArgument(std::wstring& commandLine, std::wstring_view argument);

/// @return The environment block of @p environment: `NAME=value` entries sorted by name as Windows wants
///         them (ordinal, ignoring case), each NUL-terminated, then one NUL more -- two NULs for an empty
///         one; nothing if an entry is not UTF-8.
[[nodiscard]] std::optional<std::wstring> environmentBlock(std::span<EnvironmentEntry const> environment);

/// @param environment The environment a launch was asked for.
/// @param folders The AppContainer's folders, as the parent asked the system for them.
/// @return @p environment, with every entry named as one of ContainerVariables (names compare ignoring case)
///         replaced by what that variable's row hands CreateProcessW(), in UTF-8: nothing of this process's
///         environment, or of what a caller asked for, says where a contained process keeps data.
[[nodiscard]] std::vector<EnvironmentEntry> containedEnvironment(
    std::span<EnvironmentEntry const> environment, ContainerFolders const& folders);

} // namespace vtextension::wasm::remote::process
