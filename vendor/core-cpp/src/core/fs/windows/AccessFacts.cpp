// SPDX-License-Identifier: Apache-2.0
//
// Who may reach an object on Windows: the owner and DACL of the very handle, read with
// GetSecurityInfo on a handle opened with READ_CONTROL, judged against the process token's user.
#include <core/fs/windows/AccessFacts.hpp>

#include <core/fs/AccessScope.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>

#include <aclapi.h>
#include <windows.h>

namespace core::fs::detail
{

namespace
{

    using Kind = AccessEntry::Kind;
    using Principal = AccessEntry::Principal;

    struct LocalFreeDeleter
    {
        void operator()(void* memory) const noexcept { ::LocalFree(memory); }
    };

    /// The entry types whose rights are judged; every other type is `Unknown`.
    struct EntryTypeRow
    {
        BYTE type;
        Kind kind;
    };

    constexpr auto EntryTypes = std::to_array<EntryTypeRow>({
        { .type = ACCESS_ALLOWED_ACE_TYPE, .kind = Kind::Allow },
        { .type = ACCESS_DENIED_ACE_TYPE, .kind = Kind::Deny },
    });

    /// The well-known principals that can reach every object anyway.
    struct WellKnownRow
    {
        WELL_KNOWN_SID_TYPE sid;
        Principal principal;
    };

    constexpr auto WellKnownPrincipals = std::to_array<WellKnownRow>({
        { .sid = WinLocalSystemSid, .principal = Principal::System },
        { .sid = WinBuiltinAdministratorsSid, .principal = Principal::Administrators },
    });

    [[nodiscard]] Principal principalOf(PSID sid, PSID user) noexcept
    {
        if (::EqualSid(sid, user) != FALSE)
            return Principal::Owner;
        auto const found = std::ranges::find_if(WellKnownPrincipals, [sid](WellKnownRow const& row) {
            return ::IsWellKnownSid(sid, row.sid) != FALSE;
        });
        return found != WellKnownPrincipals.end() ? found->principal : Principal::Other;
    }

    /// @return What @p header's entry is; its SID and mask are read only for the two judged types,
    ///         which share ACCESS_ALLOWED_ACE's layout.
    [[nodiscard]] AccessEntry entryOf(ACE_HEADER const* header, PSID user) noexcept
    {
        auto const origin = (header->AceFlags & INHERITED_ACE) != 0 ? AccessEntry::Origin::Inherited
                                                                    : AccessEntry::Origin::Explicit;
        auto const type = std::ranges::find(EntryTypes, header->AceType, &EntryTypeRow::type);
        if (type == EntryTypes.end())
            return AccessEntry { .kind = Kind::Unknown,
                                 .principal = Principal::Other,
                                 .rights = AccessEntry::Rights::ReadOrWrite,
                                 .origin = origin };
        auto const* const entry = reinterpret_cast<ACCESS_ALLOWED_ACE const*>(header);
        auto* const sid = const_cast<DWORD*>(&entry->SidStart);
        return AccessEntry { .kind = type->kind,
                             .principal = principalOf(sid, user),
                             .rights = entry->Mask != 0 ? AccessEntry::Rights::ReadOrWrite
                                                        : AccessEntry::Rights::None,
                             .origin = origin };
    }

} // namespace

std::optional<AccessFacts> accessFactsOf(HANDLE handle, PSID user)
{
    auto owner = PSID {};
    auto dacl = PACL {};
    auto descriptor = PSECURITY_DESCRIPTOR {};
    if (::GetSecurityInfo(handle,
                          SE_FILE_OBJECT,
                          OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                          &owner,
                          nullptr,
                          &dacl,
                          nullptr,
                          &descriptor)
        != ERROR_SUCCESS)
        return std::nullopt;
    auto const release = std::unique_ptr<void, LocalFreeDeleter> { descriptor };
    auto facts = AccessFacts { .owner = owner != nullptr && ::EqualSid(owner, user) != FALSE
                                            ? AccessFacts::Owner::CurrentUser
                                            : AccessFacts::Owner::Other,
                               .posixMode = 0,
                               .acl = AccessFacts::AclState::Null,
                               .entries = {} };
    if (dacl == nullptr)
        return facts;
    auto information = ACL_SIZE_INFORMATION {};
    if (::GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation) == FALSE)
        return std::nullopt;
    facts.acl = AccessFacts::AclState::Present;
    facts.entries.reserve(information.AceCount);
    for (auto const index: std::views::iota(DWORD { 0 }, information.AceCount))
    {
        auto* entry = LPVOID {};
        if (::GetAce(dacl, index, &entry) == FALSE)
            return std::nullopt;
        auto const* const header = static_cast<ACE_HEADER const*>(entry);
        // An inherit-only entry grants nothing on this object, only on what is created inside it.
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
            continue;
        facts.entries.push_back(entryOf(header, user));
    }
    return facts;
}

} // namespace core::fs::detail
