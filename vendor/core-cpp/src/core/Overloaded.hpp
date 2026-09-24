// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace core
{

/// Combines lambdas into one callable overload set, for std::visit:
///
/// ```cpp
/// std::visit(core::Overloaded { [](int i) { ... }, [](std::string const& s) { ... } }, value);
/// ```
template <class... Ts>
struct Overloaded: Ts...
{
    using Ts::operator()...;
};

template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace core
