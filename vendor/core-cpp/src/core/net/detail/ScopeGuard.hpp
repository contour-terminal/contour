// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A minimal scope guard: run a callable when the enclosing scope exits.
///
/// `net` keeps its own rather than reaching for a utility library's, so the module
/// stays dependency-free and can be consumed by other projects without dragging
/// Contour's `crispy` along. Being a template, it also stores the callable inline —
/// no `std::function` allocation on a path that runs per TLS handshake.

#include <type_traits>
#include <utility>

namespace core::net::detail
{

/// Invokes a callable on destruction.
///
/// Deliberately immovable: a guard exists to fire exactly once, at the end of the
/// scope that declared it. Construct it with a lambda and let it go out of scope.
/// @tparam Callable The callable to invoke; must be nothrow-invocable, since it
///         runs from a destructor and an escaping exception would terminate.
///
/// **Mark the lambda `noexcept` or this will not compile, and the diagnostic will not say why.**
/// A lambda is only nothrow-invocable if it says so, whatever its body does, so an unmarked one
/// fails the constraint -- and the failure surfaces as *"no viable constructor or deduction guide
/// for deduction of template arguments"*, which reports that the call did not match without
/// naming the requirement it missed. `auto const g = ScopeGuard { [&]() noexcept { ... } };`
template <typename Callable>
    requires std::is_nothrow_invocable_v<Callable&>
class ScopeGuard
{
  public:
    /// @param callable The action to run when this guard is destroyed.
    explicit ScopeGuard(Callable callable) noexcept(std::is_nothrow_move_constructible_v<Callable>):
        _callable(std::move(callable))
    {
    }

    ScopeGuard(ScopeGuard const&) = delete;
    ScopeGuard& operator=(ScopeGuard const&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

    /// Runs the action. Explicitly `noexcept` to state the intent the `requires`
    /// clause enforces: a throwing action would terminate from here.
    ~ScopeGuard() noexcept { _callable(); }

  private:
    Callable _callable; ///< The action run on scope exit.
};

} // namespace core::net::detail
