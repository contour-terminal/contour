// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>

namespace contour::actions
{

// This translation unit holds the copy of Action and nothing else that instantiates the variant's
// machinery over every alternative; see the comment at Action::Action() in Actions.cpp for why.
Action::Action(Action const& other) = default;

Action& Action::operator=(Action const& other)
{
    if (this != &other)
    {
        // Copy first, so a throwing copy leaves this action untouched.
        auto copy = Action(other);
        replaceWith(std::move(copy));
    }
    return *this;
}

} // namespace contour::actions
