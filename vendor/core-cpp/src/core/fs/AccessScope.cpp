// SPDX-License-Identifier: Apache-2.0
#include <core/fs/AccessScope.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace core::fs
{

namespace
{

    using Kind = AccessEntry::Kind;
    using Principal = AccessEntry::Principal;
    using Rights = AccessEntry::Rights;
    using AclState = AccessFacts::AclState;

    /// The mode bits of the group and of others: any one of them lets someone else in.
    constexpr auto SharedModeBits = std::uint32_t { 0077 };

    /// The principals an ALLOW entry may name and leave the object owner-only: the user, and the two
    /// that can reach every object whatever its DACL says.
    constexpr auto OwnerSidePrincipals =
        std::to_array({ Principal::Owner, Principal::System, Principal::Administrators });

    /// @return Whether @p entry lets someone other than the owner side in.
    [[nodiscard]] constexpr bool widens(AccessEntry const& entry) noexcept
    {
        switch (entry.kind)
        {
            case Kind::Deny: return false;
            case Kind::Unknown: return true;
            case Kind::Allow:
                return entry.rights != Rights::None
                       && std::ranges::find(OwnerSidePrincipals, entry.principal)
                              == OwnerSidePrincipals.end();
        }
        return true;
    }

} // namespace

AccessScope classifyAccess(AccessFacts const& facts) noexcept
{
    if (facts.owner != AccessFacts::Owner::CurrentUser)
        return AccessScope::ForeignOwner;
    if ((facts.posixMode & SharedModeBits) != 0)
        return AccessScope::Shared;
    switch (facts.acl)
    {
        case AclState::Absent:
        case AclState::Trivial: return AccessScope::OwnerOnly;
        case AclState::Present:
            return std::ranges::any_of(facts.entries, widens) ? AccessScope::Shared : AccessScope::OwnerOnly;
        case AclState::Null: return AccessScope::Shared;
    }
    return AccessScope::Shared;
}

} // namespace core::fs
