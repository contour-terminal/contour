// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>

#include <memory>
#include <utility>

namespace contour::actions
{

// This translation unit holds the move of Action and nothing else that instantiates the variant's
// machinery over every alternative; see the comment at Action::Action() in Actions.cpp for why.
Action::Action(Action&& other) noexcept = default;

// The assignments destroy the held alternative and construct the new one in its place, instead of
// defaulting to std::variant's own: libstdc++ implements those by emplacing each alternative, and each
// emplace resets (destroys) through a visit table of its own, which is N*N destructor call sites for the
// N alternatives. At 154 alternatives that took GCC 15 more than 11 GB to compile at -O2. Destroy and
// construct is N+N, and the variant's move constructor is noexcept in practice, so nothing is lost.
Action& Action::operator=(Action&& other) noexcept
{
    if (this != &other)
        replaceWith(std::move(other));
    return *this;
}

void Action::replaceWith(Action&& other) noexcept
{
    // The whole Action is replaced, never only its Variant base: a base-class subobject is
    // potentially-overlapping and so not transparently replaceable ([basic.life]), whereas an Action is
    // always a complete object or a plain member. Nothing derives from Action, which keeps that true.
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
}

} // namespace contour::actions
