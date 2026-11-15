// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/StepQueue.hpp>

#include <utility>
#include <variant>

namespace orchestrator::proto
{

void StepQueue::push(Input input)
{
    _inputs.push_back(std::move(input));
}

void StepQueue::carriedOut(Input const& fed)
{
    if (std::holds_alternative<Wake>(fed))
        _listing = Listing::Wanted;
}

NextStep StepQueue::next() const noexcept
{
    if (!_inputs.empty())
        return NextStep::Feed;
    return _listing == Listing::Wanted ? NextStep::ListPanes : NextStep::Idle;
}

Input StepQueue::take()
{
    auto input = std::move(_inputs.front());
    _inputs.pop_front();
    return input;
}

} // namespace orchestrator::proto
