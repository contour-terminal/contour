// SPDX-License-Identifier: Apache-2.0
#include <core/fs/detail/Ancestry.hpp>

#include <core/fs/Descriptor.hpp>

#include <algorithm>
#include <span>

namespace core::fs::detail
{

namespace
{

    [[nodiscard]] bool holds(std::span<ObjectIdentity const> ancestors,
                             ObjectIdentity const& identity) noexcept
    {
        // find_if, not find: the MSVC STL hands find over a 24-byte trivially comparable type to a
        // vectorized search that refuses its size at compile time.
        return std::ranges::find_if(ancestors,
                                    [&](ObjectIdentity const& ancestor) { return ancestor == identity; })
               != ancestors.end();
    }

} // namespace

Relation decideRelation(std::span<ObjectIdentity const> first,
                        std::span<ObjectIdentity const> second) noexcept
{
    if (first.front() == second.front())
        return Relation::Same;
    if (holds(second.subspan(1), first.front()))
        return Relation::FirstContainsSecond;
    if (holds(first.subspan(1), second.front()))
        return Relation::SecondContainsFirst;
    return Relation::Unrelated;
}

} // namespace core::fs::detail
