// SPDX-License-Identifier: Apache-2.0
//
// What BeneathStrategy::Best is on Windows: the walk, since NtCreateFile resolves a multi-component
// name with no beneath mode, so every component is opened relative to the handle before it. CMake
// compiles exactly one BeneathDefault.cpp, so no `#ifdef` chooses the strategy.
#include <core/fs/detail/ResolveBeneath.hpp>

#include <memory>

namespace core::fs::detail
{

std::unique_ptr<IBeneathOpener const> makeBestBeneathOpener()
{
    return nullptr;
}

} // namespace core::fs::detail
