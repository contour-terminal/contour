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
                return ::CreateAppContainerProfile(name, displayName, description, nullptr, 0, sid);
            },
        .deriveSid = [](PCWSTR name,
                        PSID* sid) { return ::DeriveAppContainerSidFromAppContainerName(name, sid); },
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

std::expected<PSID, LaunchRefusal> AppContainerProvision::prepare(std::filesystem::path const& directory)
{
    if (!_sid)
    {
        auto sid = ensureAppContainer(_api);
        if (!sid)
            return std::unexpected(sid.error());
        _sid = *std::move(sid);
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
    return _sid->get();
}

} // namespace vtextension::wasm::remote::process
