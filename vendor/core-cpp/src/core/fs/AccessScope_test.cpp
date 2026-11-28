// SPDX-License-Identifier: Apache-2.0
//
// The access classifier is pure, so every operating system's row of facts is held to it on every
// operating system: what a Linux fstat, a FreeBSD NFSv4 ACL, a macOS extended ACL or a Windows DACL
// would make of an object.
#include <core/fs/AccessScope.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

using core::fs::AccessEntry;
using core::fs::AccessFacts;
using core::fs::AccessScope;
using core::fs::classifyAccess;

namespace
{

using Kind = AccessEntry::Kind;
using Principal = AccessEntry::Principal;
using Rights = AccessEntry::Rights;
using Origin = AccessEntry::Origin;
using Owner = AccessFacts::Owner;
using AclState = AccessFacts::AclState;

struct Row
{
    std::string_view name;
    AccessFacts facts;
    AccessScope expected;
};

[[nodiscard]] AccessEntry allow(Principal principal, Origin origin = Origin::Explicit)
{
    return AccessEntry {
        .kind = Kind::Allow, .principal = principal, .rights = Rights::ReadOrWrite, .origin = origin
    };
}

[[nodiscard]] AccessFacts posix(std::uint32_t mode, Owner owner = Owner::CurrentUser)
{
    return AccessFacts { .owner = owner, .posixMode = mode, .acl = AclState::Absent, .entries = {} };
}

/// Windows has no mode: its facts are the owner and the DACL's entries.
[[nodiscard]] AccessFacts windows(std::vector<AccessEntry> entries, Owner owner = Owner::CurrentUser)
{
    return AccessFacts {
        .owner = owner, .posixMode = 0, .acl = AclState::Present, .entries = std::move(entries)
    };
}

} // namespace

TEST_CASE("classifyAccess follows the table", "[fs][access]")
{
    auto const row = GENERATE(
        Row { "POSIX 0600, owned by the user", posix(0600), AccessScope::OwnerOnly },
        Row { "POSIX 0700 directory", posix(040700), AccessScope::OwnerOnly },
        Row { "POSIX 0640: the group may read", posix(0640), AccessScope::Shared },
        Row { "POSIX 0602: others may write", posix(0602), AccessScope::Shared },
        Row { "POSIX 0620: the group may write", posix(0620), AccessScope::Shared },
        Row { "POSIX 0601: others may search", posix(0601), AccessScope::Shared },
        Row { "POSIX 0600 owned by another uid", posix(0600, Owner::Other), AccessScope::ForeignOwner },
        Row { "POSIX 0644 owned by another uid", posix(0644, Owner::Other), AccessScope::ForeignOwner },
        Row { "FreeBSD 0600 with a trivial NFSv4 ACL",
              AccessFacts {
                  .owner = Owner::CurrentUser, .posixMode = 0600, .acl = AclState::Trivial, .entries = {} },
              AccessScope::OwnerOnly },
        Row { "FreeBSD 0600 with a non-trivial NFSv4 ACL",
              AccessFacts { .owner = Owner::CurrentUser,
                            .posixMode = 0600,
                            .acl = AclState::Present,
                            .entries = { AccessEntry {} } },
              AccessScope::Shared },
        Row { "macOS 0600 with an extended ACL entry",
              AccessFacts { .owner = Owner::CurrentUser,
                            .posixMode = 0600,
                            .acl = AclState::Present,
                            .entries = { AccessEntry { .kind = Kind::Unknown,
                                                       .principal = Principal::Other,
                                                       .rights = Rights::ReadOrWrite,
                                                       .origin = Origin::Explicit } } },
              AccessScope::Shared },
        Row {
            "Windows NULL DACL",
            AccessFacts { .owner = Owner::CurrentUser, .posixMode = 0, .acl = AclState::Null, .entries = {} },
            AccessScope::Shared },
        Row { "Windows empty DACL: nobody but the owner", windows({}), AccessScope::OwnerOnly },
        Row { "Windows owner only", windows({ allow(Principal::Owner) }), AccessScope::OwnerOnly },
        Row {
            "Windows SYSTEM, Administrators and the owner",
            windows({ allow(Principal::System), allow(Principal::Administrators), allow(Principal::Owner) }),
            AccessScope::OwnerOnly },
        Row { "Windows the same, inherited",
              windows({ allow(Principal::System, Origin::Inherited),
                        allow(Principal::Administrators, Origin::Inherited),
                        allow(Principal::Owner, Origin::Inherited) }),
              AccessScope::OwnerOnly },
        Row { "Windows Everyone ALLOW, explicit",
              windows({ allow(Principal::Owner), allow(Principal::Other) }),
              AccessScope::Shared },
        Row { "Windows Everyone ALLOW, inherited",
              windows({ allow(Principal::Owner), allow(Principal::Other, Origin::Inherited) }),
              AccessScope::Shared },
        Row { "Windows an ALLOW of no right to another SID",
              windows({ allow(Principal::Owner),
                        AccessEntry { .kind = Kind::Allow,
                                      .principal = Principal::Other,
                                      .rights = Rights::None,
                                      .origin = Origin::Explicit } }),
              AccessScope::OwnerOnly },
        Row { "Windows DENY Everyone never widens",
              windows({ allow(Principal::Owner),
                        AccessEntry { .kind = Kind::Deny,
                                      .principal = Principal::Other,
                                      .rights = Rights::ReadOrWrite,
                                      .origin = Origin::Explicit } }),
              AccessScope::OwnerOnly },
        Row { "Windows an entry of unknown type",
              windows({ allow(Principal::Owner),
                        AccessEntry { .kind = Kind::Unknown,
                                      .principal = Principal::Owner,
                                      .rights = Rights::ReadOrWrite,
                                      .origin = Origin::Explicit } }),
              AccessScope::Shared },
        Row { "Windows owned by Administrators, not the user",
              windows({ allow(Principal::Owner) }, Owner::Other),
              AccessScope::ForeignOwner },
        Row { "default facts fail closed", AccessFacts {}, AccessScope::ForeignOwner });
    INFO(row.name);
    CHECK(classifyAccess(row.facts) == row.expected);
}

TEST_CASE("a default AccessScope is Shared", "[fs][access]")
{
    // A zero-initialised scope must never read as owner-only.
    CHECK(AccessScope {} == AccessScope::Shared);
}
