// SPDX-License-Identifier: Apache-2.0
//
// The extension host's AppContainer: its profile, and the read and execute access every AppContainer needs
// on the helper's directory. The design follows Chromium's sandbox/win (app_container_base.cc), which also
// creates or derives one named profile and grants ALL APPLICATION PACKAGES on the files a contained process
// must load.
#include <vtextension/wasm/remote/process/AppContainer_win32.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <AclAPI.h>
#include <combaseapi.h>
#include <sddl.h>
#include <userenv.h>

namespace vtextension::wasm::remote::process
{

namespace
{
    /// Frees what the system allocated with LocalAlloc().
    struct LocalMemoryFree
    {
        void operator()(void* memory) const noexcept { ::LocalFree(memory); }
    };
    using LocalMemory = std::unique_ptr<void, LocalMemoryFree>;

    /// The system functions the container is ensured through beyond kernel32's and advapi32's, looked up from
    /// System32 the first time a launcher prepares a container, and imported by no executable -- neither as
    /// it starts nor delay-loaded. An executable that links the launcher may itself run contained (the probe
    /// does), and ole32, CoTaskMemFree()'s usual home, loads user32, whose initialisation connects to a
    /// window station that a service's (a CI runner's, an SSH session's) does not admit an app container to:
    /// the loader then ends the process with STATUS_DLL_INIT_FAILED (0xC0000142) before its main().
    /// Delay-loading would take /DELAYLOAD on exactly the executables that pull this file in, and link.exe
    /// warns on every other (LNK4199). userenv and combase are loaded once and deliberately never freed: that
    /// keeps the looked-up pointers valid for as long as the process runs.
    struct ContainerFunctions
    {
        decltype(&::CreateAppContainerProfile) createProfile = nullptr;             ///< userenv's.
        decltype(&::DeriveAppContainerSidFromAppContainerName) deriveSid = nullptr; ///< userenv's.
        decltype(&::GetAppContainerFolderPath) folderPath = nullptr;                ///< userenv's.
        decltype(&::CoTaskMemFree) freeTaskMemory = nullptr; ///< combase's, which loads no user32.
    };

    /// @return The function @p name of @p library, as @p Function; nothing if either is missing.
    template <typename Function>
    [[nodiscard]] Function lookUp(HMODULE library, char const* name) noexcept
    {
        if (library == nullptr)
            return nullptr;
        return reinterpret_cast<Function>(::GetProcAddress(library, name));
    }

    /// @return The functions, looked up once; a missing one is null, and what needs it fails with
    ///         MissingFunction.
    [[nodiscard]] ContainerFunctions const& containerFunctions() noexcept
    {
        static auto const functions = [] {
            auto* const userenv = ::LoadLibraryExW(L"userenv.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            auto* const combase = ::LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return ContainerFunctions {
                .createProfile =
                    lookUp<decltype(&::CreateAppContainerProfile)>(userenv, "CreateAppContainerProfile"),
                .deriveSid = lookUp<decltype(&::DeriveAppContainerSidFromAppContainerName)>(
                    userenv, "DeriveAppContainerSidFromAppContainerName"),
                .folderPath =
                    lookUp<decltype(&::GetAppContainerFolderPath)>(userenv, "GetAppContainerFolderPath"),
                .freeTaskMemory = lookUp<decltype(&::CoTaskMemFree)>(combase, "CoTaskMemFree"),
            };
        }();
        return functions;
    }

    /// What a call fails with whose system function could not be looked up.
    constexpr auto MissingFunction = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    /// Frees what the system allocated with CoTaskMemAlloc().
    struct TaskMemoryFree
    {
        void operator()(wchar_t* memory) const noexcept
        {
            // Only folderPath() allocates, and only with freeTaskMemory found.
            if (auto const free = containerFunctions().freeTaskMemory; free != nullptr)
                free(memory);
        }
    };
    using TaskMemory = std::unique_ptr<wchar_t, TaskMemoryFree>;

    /// What every AppContainer needs on the helper's directory, and on each file in it: read and execute, as
    /// FILE_GENERIC_READ | FILE_GENERIC_EXECUTE (`RX`) spells it -- FILE_EXECUTE the one right the second
    /// adds.
    constexpr auto ReadAndExecute = DWORD { FILE_GENERIC_READ | FILE_EXECUTE };

    /// How the generic rights of a file's ACE map to its specific ones.
    constexpr auto FileRightsMapping =
        GENERIC_MAPPING { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };

    /// What a directory's DACL entries let a SID read and execute: the directory itself, the files it gives
    /// its entries to, or both. A bitmask: the entries of one DACL add up.
    enum class Coverage : uint8_t
    {
        None = 0b00,      ///< Neither.
        Directory = 0b01, ///< The directory itself.
        Files = 0b10,     ///< The files created in it.
        Both = 0b11,      ///< Both.
    };

    [[nodiscard]] constexpr Coverage operator|(Coverage a, Coverage b) noexcept
    {
        return static_cast<Coverage>(std::to_underlying(a) | std::to_underlying(b));
    }

    /// @return What @p ace, an entry of a directory's DACL, lets @p sid read and execute.
    [[nodiscard]] Coverage coverageOf(ACE_HEADER const& ace, PSID sid) noexcept
    {
        if (ace.AceType != ACCESS_ALLOWED_ACE_TYPE)
            return Coverage::None;
        auto const& allowed = reinterpret_cast<ACCESS_ALLOWED_ACE const&>(ace);
        if (!::EqualSid(const_cast<DWORD*>(&allowed.SidStart), sid))
            return Coverage::None;
        auto mask = allowed.Mask;
        auto mapping = FileRightsMapping;
        ::MapGenericMask(&mask, &mapping);
        if ((mask & ReadAndExecute) != ReadAndExecute)
            return Coverage::None;
        auto coverage = Coverage::None;
        if ((ace.AceFlags & INHERIT_ONLY_ACE) == 0)
            coverage = coverage | Coverage::Directory;
        if ((ace.AceFlags & OBJECT_INHERIT_ACE) != 0)
            coverage = coverage | Coverage::Files;
        return coverage;
    }

    /// @return Whether @p dacl lets @p sid read and execute the directory and the files in it: a null DACL
    ///         lets everyone do anything.
    [[nodiscard]] bool grantsReadAndExecute(PACL dacl, PSID sid) noexcept
    {
        if (dacl == nullptr)
            return true;
        auto coverage = Coverage::None;
        for (auto const index: std::views::iota(DWORD { 0 }, DWORD { dacl->AceCount }))
        {
            void* ace = nullptr;
            if (::GetAce(dacl, index, &ace))
                coverage = coverage | coverageOf(*static_cast<ACE_HEADER const*>(ace), sid);
        }
        return coverage == Coverage::Both;
    }

    /// @return The SID of ALL APPLICATION PACKAGES; nothing if the system cannot spell it.
    [[nodiscard]] LocalMemory allApplicationPackages() noexcept
    {
        PSID sid = nullptr;
        if (!::ConvertStringSidToSidW(std::wstring { AllApplicationPackagesSid }.c_str(), &sid))
            return LocalMemory {};
        return LocalMemory { sid };
    }

    /// @return Why a DACL could not be read or written at @p step, as a launch failure: `NotGranted` where
    ///         this user may not change it.
    [[nodiscard]] LaunchRefusal daclRefusalOf(LaunchStep step, DWORD error) noexcept
    {
        return LaunchRefusal { .error =
                                   error == ERROR_ACCESS_DENIED ? LaunchError::NotGranted : LaunchError::Io,
                               .step = step,
                               .code = error };
    }

    /// @return @p path in UTF-8, as every line Contour logs is; path::string() would spell it in the ANSI
    ///         code page, and throw where that has no spelling.
    [[nodiscard]] std::string utf8Of(std::filesystem::path const& path)
    {
        auto const utf8 = path.u8string();
        return std::string { utf8.begin(), utf8.end() };
    }
} // namespace

AppContainerApi systemAppContainerApi()
{
    return AppContainerApi {
        .createProfile =
            [](PCWSTR name, PCWSTR displayName, PCWSTR description, PSID* sid) {
                auto const create = containerFunctions().createProfile;
                if (create == nullptr)
                    return MissingFunction;
                return create(name, displayName, description, nullptr, 0, sid);
            },
        .deriveSid =
            [](PCWSTR name, PSID* sid) {
                auto const derive = containerFunctions().deriveSid;
                if (derive == nullptr)
                    return MissingFunction;
                return derive(name, sid);
            },
        .readDacl =
            [](PCWSTR path, PACL* dacl, PSECURITY_DESCRIPTOR* descriptor) {
                return ::GetNamedSecurityInfoW(path,
                                               SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION,
                                               nullptr,
                                               nullptr,
                                               dacl,
                                               nullptr,
                                               descriptor);
            },
        .writeDacl =
            [](PCWSTR path, PACL dacl) {
                return ::SetNamedSecurityInfoW(const_cast<PWSTR>(path),
                                               SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION,
                                               nullptr,
                                               nullptr,
                                               dacl,
                                               nullptr);
            },
        .folderPath = [](PSID sid) -> std::expected<std::filesystem::path, HRESULT> {
            auto const& functions = containerFunctions();
            if (functions.folderPath == nullptr || functions.freeTaskMemory == nullptr)
                return std::unexpected(MissingFunction);
            PWSTR spelled = nullptr;
            if (!::ConvertSidToStringSidW(sid, &spelled))
                return std::unexpected(HRESULT_FROM_WIN32(::GetLastError()));
            auto const ownedSpelled = LocalMemory { spelled };
            PWSTR folder = nullptr;
            auto const result = functions.folderPath(spelled, &folder);
            auto const ownedFolder = TaskMemory { folder };
            if (FAILED(result))
                return std::unexpected(result);
            if (ownedFolder == nullptr)
                return std::filesystem::path {};
            return std::filesystem::path { ownedFolder.get() };
        },
    };
}

std::expected<OwnedSid, LaunchRefusal> ensureAppContainer(AppContainerApi const& api)
{
    auto const name = std::wstring { AppContainerName };
    PSID sid = nullptr;
    auto result = api.createProfile(
        name.c_str(), L"Contour extension host", L"Runs Contour's status-line extensions.", &sid);
    if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        result = api.deriveSid(name.c_str(), &sid);
    if (FAILED(result) || sid == nullptr)
        return std::unexpected(LaunchRefusal { .error = LaunchError::Unsupported,
                                               .step = LaunchStep::Profile,
                                               .code = static_cast<DWORD>(result) });
    return OwnedSid { sid };
}

std::expected<void, LaunchRefusal> ensurePackagesCanExecute(std::filesystem::path const& directory,
                                                            AppContainerApi const& api)
{
    auto const packages = allApplicationPackages();
    if (!packages)
        return std::unexpected(LaunchRefusal {
            .error = LaunchError::Io, .step = LaunchStep::PackagesSid, .code = ::GetLastError() });
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (auto const error = api.readDacl(directory.c_str(), &dacl, &descriptor); error != ERROR_SUCCESS)
        return std::unexpected(daclRefusalOf(LaunchStep::ReadGrant, error));
    auto const ownedDescriptor = LocalMemory { descriptor };
    if (grantsReadAndExecute(dacl, packages.get()))
        return {};

    // One entry for the directory and the files directly in it -- the helper and the libraries beside it --
    // and for nothing beneath: no propagation, so a subdirectory holds not even an inherit-only copy for its
    // own files. Nothing a contained process loads lives deeper.
    auto access = EXPLICIT_ACCESS_W {};
    access.grfAccessPermissions = ReadAndExecute;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = OBJECT_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(packages.get());
    PACL granted = nullptr;
    if (auto const error = ::SetEntriesInAclW(1, &access, dacl, &granted); error != ERROR_SUCCESS)
        return std::unexpected(
            LaunchRefusal { .error = LaunchError::Io, .step = LaunchStep::Grant, .code = error });
    auto const ownedGranted = LocalMemory { granted };
    if (auto const error = api.writeDacl(directory.c_str(), granted); error != ERROR_SUCCESS)
        return std::unexpected(daclRefusalOf(LaunchStep::WriteGrant, error));
    return {};
}

AppContainerProvision::AppContainerProvision(AppContainerApi api,
                                             std::function<void(std::string_view line)> diagnostics):
    _api { std::move(api) }, _diagnostics { std::move(diagnostics) }
{
}

std::expected<PreparedContainer, LaunchRefusal> AppContainerProvision::prepare(
    std::filesystem::path const& directory)
{
    if (!_sid)
    {
        auto sid = ensureAppContainer(_api);
        if (!sid)
            return std::unexpected(sid.error());
        _sid = *std::move(sid);
    }
    if (!_folder)
    {
        // A folder named empty names none, and one not shaped <profile>\Packages\<name>\AC yields no
        // profile folder for CreateProcessW() to extend (ContainerVariables): either is refused.
        auto folder =
            _api.folderPath(_sid->get())
                .and_then([](std::filesystem::path const& named) -> std::expected<ContainerFolders, HRESULT> {
                    if (named.empty())
                        return std::unexpected(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
                    auto folders = containerFoldersOf(named);
                    if (!folders)
                        return std::unexpected(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));
                    return *std::move(folders);
                });
        if (!folder)
            return std::unexpected(LaunchRefusal { .error = LaunchError::Io,
                                                   .step = LaunchStep::ContainerFolder,
                                                   .code = static_cast<DWORD>(folder.error()) });
        _folder = *std::move(folder);
    }
    if (!std::ranges::contains(_grantedDirectories, directory))
    {
        // Asked at every launch, as an administrator may grant it meanwhile; said once, not at every launch.
        if (auto const granted = ensurePackagesCanExecute(directory, _api); !granted)
        {
            if (granted.error().error == LaunchError::NotGranted
                && !std::ranges::contains(_ungrantableDirectories, directory))
            {
                _ungrantableDirectories.push_back(directory);
                _diagnostics(
                    std::format("The extension host in {} cannot run contained: Windows app containers "
                                "may not read and execute that directory, and this user may not "
                                "grant it. The installer must grant ALL APPLICATION PACKAGES ({}) "
                                "read and execute access there.",
                                utf8Of(directory),
                                utf8Of(std::filesystem::path { AllApplicationPackagesSid })));
            }
            return std::unexpected(granted.error());
        }
        _grantedDirectories.push_back(directory);
    }
    return PreparedContainer { .sid = _sid->get(), .folders = *_folder };
}

} // namespace vtextension::wasm::remote::process
