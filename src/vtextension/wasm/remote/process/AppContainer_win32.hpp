// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Windows only: the AppContainer the extension host is started in (ProcessIsolation::AppContainer). Its
/// profile, `Contour.ExtensionHost`, has no capabilities: no network, no user files, no registry beyond what
/// every AppContainer reads. A container reads only what grants `ALL APPLICATION PACKAGES` (S-1-15-2-1) or
/// its own SID, so the helper's directory must grant the first read and execute access before the helper can
/// start.
///
/// Both are ensured by the launcher, once per launcher -- one per supervisor, on its thread -- through an API
/// table (AppContainerApi), so that what the system answers can be faked in a test.

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/wasm/remote/process/LaunchRefusal_win32.hpp>
#include <vtextension/wasm/remote/process/LaunchSpelling_win32.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::process
{

/// The name of the extension host's AppContainer profile.
inline constexpr auto AppContainerName = std::wstring_view { L"Contour.ExtensionHost" };

/// The SID of `ALL APPLICATION PACKAGES`, which every AppContainer's token carries.
inline constexpr auto AllApplicationPackagesSid = std::wstring_view { L"S-1-15-2-1" };

/// Frees a SID that userenv returned, as its functions say: with FreeSid().
struct SidFree
{
    void operator()(void* sid) const noexcept { ::FreeSid(sid); }
};

/// A SID that userenv returned.
using OwnedSid = std::unique_ptr<void, SidFree>;

/// The userenv and advapi functions the container is ensured through, one per thing the system is asked.
struct AppContainerApi
{
    /// CreateAppContainerProfile() with no capabilities: the profile @p name, and its SID in @p sid.
    std::function<HRESULT(PCWSTR name, PCWSTR displayName, PCWSTR description, PSID* sid)> createProfile;
    /// DeriveAppContainerSidFromAppContainerName(): the SID of the existing profile @p name.
    std::function<HRESULT(PCWSTR name, PSID* sid)> deriveSid;
    /// GetNamedSecurityInfoW() of a file's DACL: the list in @p dacl, owned by @p descriptor (LocalFree()).
    std::function<DWORD(PCWSTR path, PACL* dacl, PSECURITY_DESCRIPTOR* descriptor)> readDacl;
    /// SetNamedSecurityInfoW() of a file's DACL, which propagates its inheritable entries to the children.
    std::function<DWORD(PCWSTR path, PACL dacl)> writeDacl;
    /// GetAppContainerFolderPath() of the container @p sid, spelled as ConvertSidToStringSidW() spells it:
    /// the folder it keeps its local data in; or the HRESULT the system refused with.
    std::function<std::expected<std::filesystem::path, HRESULT>(PSID sid)> folderPath;
};

/// The extension host's AppContainer, as a launch into it needs it.
struct PreparedContainer
{
    PSID sid;                 ///< Its SID, owned by the provision that prepared it.
    ContainerFolders folders; ///< Its own folder (AppContainerApi::folderPath), and the profile's it lies in.

    bool operator==(PreparedContainer const&) const = default;
};

/// @return The system's functions.
[[nodiscard]] AppContainerApi systemAppContainerApi();

/// Creates the extension host's profile (AppContainerName), with no capabilities, or finds it already there.
/// @param api What the system is asked through.
/// @return Its SID; `Unsupported` if the system neither created nor had it, with the HRESULT it said.
[[nodiscard]] std::expected<OwnedSid, LaunchRefusal> ensureAppContainer(AppContainerApi const& api);

/// Lets every AppContainer read and execute @p directory and the files in it, unless its DACL lets them
/// already -- whether by an entry of its own or an inherited one, as `Program Files` does. What it adds
/// reaches the files directly in @p directory, and nothing beneath it.
/// @param directory The helper's directory.
/// @param api What the system is asked through.
/// @return Nothing once they may; `NotGranted` if this user may not change the DACL (an installation it does
///         not own, whose installer must grant it), `Io` if the system refused otherwise; each with the step
///         refused and its code.
[[nodiscard]] std::expected<void, LaunchRefusal> ensurePackagesCanExecute(
    std::filesystem::path const& directory, AppContainerApi const& api);

/// What a launcher starts contained processes with: the profile, ensured on first use, and each executable
/// directory's grant, ensured once. Used by one thread at a time (the supervisor's).
class AppContainerProvision
{
  public:
    /// @param api What the system is asked through.
    /// @param diagnostics Where a grant this user may not make is said, with the directory and the fix:
    ///        Contour's error log.
    AppContainerProvision(AppContainerApi api, std::function<void(std::string_view line)> diagnostics);

    /// Ensures the profile and asks for its folder, once, and that executables in @p directory may run in
    /// it, once per directory. A directory this user may not grant is asked of again at every launch, but
    /// said only once.
    /// @param directory The directory of the executable about to be started.
    /// @return The container; why it cannot be used, and where the system refused: a folder it was refused,
    ///         named empty (ERROR_PATH_NOT_FOUND), or not shaped as a container's (ERROR_BAD_PATHNAME), is
    ///         `Io` at LaunchStep::ContainerFolder, with the HRESULT.
    [[nodiscard]] std::expected<PreparedContainer, LaunchRefusal> prepare(
        std::filesystem::path const& directory);

  private:
    AppContainerApi _api;
    std::function<void(std::string_view line)> _diagnostics;
    std::optional<OwnedSid> _sid;                           ///< Once ensured.
    std::optional<ContainerFolders> _folder;                ///< Once the system named it.
    std::vector<std::filesystem::path> _grantedDirectories; ///< Each directory ensured.
    std::vector<std::filesystem::path>
        _ungrantableDirectories; ///< Each directory refused `NotGranted`, said once.
};

} // namespace vtextension::wasm::remote::process
