// SPDX-License-Identifier: Apache-2.0
//
// The read and execute access every AppContainer needs on the extension host's directory. Chromium's
// sandbox/win (app_container_base.cc) likewise grants ALL APPLICATION PACKAGES on the files a contained
// process must load.
#include <vtextension/wasm/remote/process/AppContainer_win32.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

#include <AclAPI.h>
#include <sddl.h>

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

    /// @return Why a DACL could not be read or written, as a launch failure: `Unsupported` where this user
    ///         may not change it.
    [[nodiscard]] LaunchError daclErrorOf(DWORD error) noexcept
    {
        return error == ERROR_ACCESS_DENIED ? LaunchError::Unsupported : LaunchError::Io;
    }
} // namespace

AppContainerApi systemAppContainerApi()
{
    return AppContainerApi {
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

std::expected<void, LaunchError> ensurePackagesCanExecute(std::filesystem::path const& directory,
                                                          AppContainerApi const& api)
{
    auto const packages = allApplicationPackages();
    if (!packages)
        return std::unexpected(LaunchError::Io);
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (auto const error = api.readDacl(directory.c_str(), &dacl, &descriptor); error != ERROR_SUCCESS)
        return std::unexpected(daclErrorOf(error));
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
        return std::unexpected(LaunchError::Io);
    auto const ownedGranted = LocalMemory { granted };
    if (auto const error = api.writeDacl(directory.c_str(), granted); error != ERROR_SUCCESS)
        return std::unexpected(daclErrorOf(error));
    return {};
}

} // namespace vtextension::wasm::remote::process
