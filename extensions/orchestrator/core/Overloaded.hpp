// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace orchestrator
{

/// Combines lambdas into one overload set for std::visit. core::base has one too, but the core links nothing
/// of core-cpp yet; this one goes when it may.
template <typename... Ts>
struct Overloaded: Ts...
{
    using Ts::operator()...;
};

} // namespace orchestrator
