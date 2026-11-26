// SPDX-License-Identifier: Apache-2.0
//
// What an AppContainer needs of the extension host's directory, on Windows: the grant is made once, reaches
// the directory's files and not the directories beneath, and is seen where it is inherited.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

#ifdef _WIN32
    #include <algorithm>
    #include <array>
    #include <format>
    #include <ranges>

    #include <vtextension/wasm/remote/process/AppContainer_win32.hpp>

    #define WIN32_LEAN_AND_MEAN
    #include <AclAPI.h>
    #include <Windows.h>
    #include <sddl.h>
#endif

namespace process = vtextension::wasm::remote::process;

#ifdef _WIN32
namespace
{

/// A fresh directory under the temporary directory, removed when destroyed.
struct ScratchDirectory
{
    std::filesystem::path path;

    explicit ScratchDirectory(std::string_view name):
        path { std::filesystem::temp_directory_path()
               / std::format("contour-{}-{}", name, process::currentProcessId()) }
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    ~ScratchDirectory()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(path, error);
    }
};

/// The SID of ALL APPLICATION PACKAGES, freed with LocalFree().
struct PackagesSid
{
    PSID sid = nullptr;

    PackagesSid()
    {
        ::ConvertStringSidToSidW(std::wstring { process::AllApplicationPackagesSid }.c_str(), &sid);
    }
    PackagesSid(PackagesSid const&) = delete;
    PackagesSid(PackagesSid&&) = delete;
    PackagesSid& operator=(PackagesSid const&) = delete;
    PackagesSid& operator=(PackagesSid&&) = delete;
    ~PackagesSid() { ::LocalFree(sid); }
};

/// Gives @p directory a DACL of its own, inheriting nothing: full control for this user, inherited by
/// everything beneath.
/// @return Whether it has one.
[[nodiscard]] bool protectDirectory(std::filesystem::path const& directory)
{
    auto currentUser = std::wstring { L"CURRENT_USER" };
    auto owner = EXPLICIT_ACCESS_W {};
    owner.grfAccessPermissions = GENERIC_ALL;
    owner.grfAccessMode = SET_ACCESS;
    owner.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    owner.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    owner.Trustee.TrusteeType = TRUSTEE_IS_USER;
    owner.Trustee.ptstrName = currentUser.data();
    PACL dacl = nullptr;
    if (::SetEntriesInAclW(1, &owner, nullptr, &dacl) != ERROR_SUCCESS)
        return false;
    auto path = directory.wstring();
    auto const result =
        ::SetNamedSecurityInfoW(path.data(),
                                SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr,
                                nullptr,
                                dacl,
                                nullptr);
    ::LocalFree(dacl);
    return result == ERROR_SUCCESS;
}

/// One entry for ALL APPLICATION PACKAGES in a DACL a test builds.
struct PackageEntry
{
    BYTE flags; ///< Its ACE flags.
    DWORD mask; ///< Its access mask.
};

/// A DACL built in memory from entries for ALL APPLICATION PACKAGES: what a directory's would read as.
class BuiltDacl
{
  public:
    explicit BuiltDacl(std::span<PackageEntry const> entries): _storage(1024)
    {
        auto* const acl = reinterpret_cast<PACL>(_storage.data());
        ::InitializeAcl(acl, static_cast<DWORD>(_storage.size()), ACL_REVISION);
        for (auto const& entry: entries)
            ::AddAccessAllowedAceEx(acl, ACL_REVISION, entry.flags, entry.mask, _packages.sid);
    }

    [[nodiscard]] PACL acl() noexcept { return reinterpret_cast<PACL>(_storage.data()); }

  private:
    std::vector<std::byte> _storage;
    PackagesSid _packages;
};

/// @return The flags of each entry of @p directory's DACL that is its own (not inherited) and allows ALL
///         APPLICATION PACKAGES; nothing if it cannot be read.
[[nodiscard]] std::optional<std::vector<BYTE>> packageEntriesOf(std::filesystem::path const& directory)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(directory.c_str(),
                                SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                nullptr,
                                nullptr,
                                &dacl,
                                nullptr,
                                &descriptor)
        != ERROR_SUCCESS)
        return std::nullopt;
    auto const packages = PackagesSid {};
    auto flags = std::vector<BYTE> {};
    for (auto const index:
         std::views::iota(DWORD { 0 }, dacl != nullptr ? DWORD { dacl->AceCount } : DWORD { 0 }))
    {
        void* ace = nullptr;
        if (!::GetAce(dacl, index, &ace))
            continue;
        auto const& header = *static_cast<ACE_HEADER const*>(ace);
        auto* const allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (header.AceType == ACCESS_ALLOWED_ACE_TYPE && (header.AceFlags & INHERITED_ACE) == 0
            && ::EqualSid(&allowed->SidStart, packages.sid))
            flags.push_back(header.AceFlags);
    }
    ::LocalFree(descriptor);
    return flags;
}

/// @return Each entry of @p directory's DACL as `type flags mask SID`, one per line: what a failing case
///         shows.
[[nodiscard]] std::string daclText(std::filesystem::path const& directory)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(directory.c_str(),
                                SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                nullptr,
                                nullptr,
                                &dacl,
                                nullptr,
                                &descriptor)
        != ERROR_SUCCESS)
        return "unreadable";
    if (dacl == nullptr)
    {
        ::LocalFree(descriptor);
        return "no DACL";
    }
    auto text = std::string {};
    for (auto const index: std::views::iota(DWORD { 0 }, DWORD { dacl->AceCount }))
    {
        void* ace = nullptr;
        if (!::GetAce(dacl, index, &ace))
            continue;
        auto const& header = *static_cast<ACE_HEADER const*>(ace);
        auto* const allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        LPWSTR sid = nullptr;
        auto name = std::string { "?" };
        if (::ConvertSidToStringSidW(&allowed->SidStart, &sid))
        {
            auto const utf8 = std::filesystem::path { sid }.u8string();
            name.assign(utf8.begin(), utf8.end());
            ::LocalFree(sid);
        }
        text += std::format("type {} flags 0x{:02X} mask 0x{:08X} {}\n",
                            header.AceType,
                            header.AceFlags,
                            allowed->Mask,
                            name);
    }
    ::LocalFree(descriptor);
    return text;
}

/// @return The system's API, except that a DACL write counts into @p writes and fails: for a check that
///         none happens.
[[nodiscard]] process::AppContainerApi apiThatMustNotWrite(std::shared_ptr<std::size_t> const& writes)
{
    auto api = process::systemAppContainerApi();
    api.writeDacl = [writes](PCWSTR /*path*/, PACL /*dacl*/) {
        ++*writes;
        return DWORD { ERROR_ACCESS_DENIED };
    };
    return api;
}

} // namespace

TEST_CASE("AppContainer.packageGrantIsIdempotent", "[vtextension][appcontainer]")
{
    auto const directory = ScratchDirectory { "package-grant" };
    REQUIRE(protectDirectory(directory.path));
    auto const before = packageEntriesOf(directory.path);
    REQUIRE(before.has_value());
    CHECK(before->empty());

    SECTION("granted once, to the directory and its files; then never written again")
    {
        CHECK(
            process::ensurePackagesCanExecute(directory.path, process::systemAppContainerApi()).has_value());
        INFO(daclText(directory.path));
        auto const after = packageEntriesOf(directory.path);
        REQUIRE(after.has_value());
        // One entry, inherited by the files directly in it only: nothing beneath is reached, not even as an
        // inherit-only copy on a subdirectory.
        CHECK(*after == std::vector<BYTE> { OBJECT_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE });

        // The grant is seen: a second call writes nothing, as its write would fail.
        auto const writes = std::make_shared<std::size_t>(0);
        CHECK(process::ensurePackagesCanExecute(directory.path, apiThatMustNotWrite(writes)).has_value());
        CHECK(*writes == 0);
    }
    SECTION("a directory this user may not change: unsupported, which the installer must grant")
    {
        auto const writes = std::make_shared<std::size_t>(0);
        auto const api = apiThatMustNotWrite(writes);
        auto const granted = process::ensurePackagesCanExecute(directory.path, api);
        REQUIRE_FALSE(granted.has_value());
        CHECK(granted.error() == process::LaunchError::Unsupported);
        CHECK(*writes == 1);
    }
}

TEST_CASE("AppContainer.programFilesGrantIsSeen", "[vtextension][appcontainer]")
{
    // What a directory under `Program Files` inherits for ALL APPLICATION PACKAGES -- `(I)(RX)` for itself
    // and
    // `(I)(OI)(CI)(IO)(GR,GE)` for what it holds -- is a grant already: nothing is written, which an
    // installation this user does not own could not take. Built in memory and read through the API, as not
    // every system computes inheritance on creation (wine does not). Either entry alone is not enough.
    struct Row
    {
        std::string_view name;
        std::vector<PackageEntry> entries;
        std::size_t writes; ///< 0: seen as granted; 1: a grant is written.
    };
    constexpr auto Itself = PackageEntry { .flags = INHERITED_ACE, .mask = FILE_GENERIC_READ | FILE_EXECUTE };
    constexpr auto ItsFiles =
        PackageEntry { .flags = INHERITED_ACE | OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE,
                       .mask = GENERIC_READ | GENERIC_EXECUTE };
    auto const rows = std::array {
        Row { .name = "both", .entries = { Itself, ItsFiles }, .writes = 0 },
        Row { .name = "itself only", .entries = { Itself }, .writes = 1 },
        Row { .name = "its files only", .entries = { ItsFiles }, .writes = 1 },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto dacl = std::make_shared<BuiltDacl>(row.entries);
        auto const writes = std::make_shared<std::size_t>(0);
        auto api = apiThatMustNotWrite(writes);
        api.readDacl = [dacl](PCWSTR /*path*/, PACL* read, PSECURITY_DESCRIPTOR* descriptor) {
            *read = dacl->acl();
            *descriptor = nullptr;
            return DWORD { ERROR_SUCCESS };
        };
        CHECK(process::ensurePackagesCanExecute(std::filesystem::temp_directory_path(), api).has_value()
              == (row.writes == 0));
        CHECK(*writes == row.writes);
    }
}

#endif
