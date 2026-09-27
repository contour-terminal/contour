// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Concepts shared across @c core::async awaitables and coroutine promises.
///
/// It sits below @c Cancellation.hpp rather than beside it: @c HasStopToken is the contract every
/// templated `await_suspend` in this module reads the awaiting promise through, so the header that
/// states it may depend on nothing of the module but @c StopToken.

#include <core/async/StopToken.hpp>

#include <concepts>
#include <coroutine>

namespace core::async
{

/// Satisfied by a type that can drive a `co_await` expression directly, i.e. it
/// exposes the awaiter interface (`await_ready`, `await_suspend`, `await_resume`).
///
/// `await_suspend` is intentionally not constrained here because it may legally
/// return `void`, `bool`, or a `std::coroutine_handle<>` (symmetric transfer).
/// @tparam A The candidate awaiter type.
template <typename A>
concept Awaiter = requires(A awaiter, std::coroutine_handle<> continuation) {
    { awaiter.await_ready() } -> std::convertible_to<bool>;
    awaiter.await_suspend(continuation);
    awaiter.await_resume();
};

/// Satisfied by a coroutine promise that carries a cancellation @c StopToken.
///
/// Runtime awaitables read the awaiting coroutine's token through this interface
/// in their templated `await_suspend`, so cancellation propagates down a
/// structured chain of `co_await`s without threading a token through every call.
/// @tparam P The candidate promise type.
template <typename P>
concept HasStopToken = requires(P& promise) {
    { promise.stopToken() } -> std::convertible_to<StopToken>;
};

/// Satisfied by a coroutine promise that carries the root of the await chain it belongs to, for
/// the case where that chain belongs to nobody.
///
/// It is the second thing a templated `await_suspend` reads the awaiting promise through
/// (@c HasStopToken being the first): `core::async::detail::unownedRootOf` answers *may an
/// executor free what it is holding* from it, and a promise that does not carry the answer says
/// "not mine", which is what a coroutine type this module does not know should say.
/// @tparam P The candidate promise type.
template <typename P>
concept CarriesUnownedRoot = requires(P& promise) {
    { promise.unownedRoot } -> std::convertible_to<std::coroutine_handle<>>;
};

namespace detail
{
    /// An awaiter of type @p A that nobody constructs, declared so a compile-time check can ask
    /// what its `await_ready` answers without making one. Never defined, and never odr-used:
    /// @c awaitReadyIsConstantFalse only names it inside a constant expression.
    template <typename A>
    extern A const& unconstructedAwaiter;

    /// Whether this compiler evaluates a member call through a reference to an object it knows
    /// nothing about in a constant expression (P2280, part of C++23).
    ///
    /// **A compiler-capability check, not platform logic**: the `#if` below asks which compiler
    /// this is, never which operating system, and changes no behaviour -- only whether a
    /// compile-time assertion can be evaluated. Measured: GCC 14, Clang 22, clang-cl 22 and MSVC
    /// 19.51 evaluate it, and fail to compile a call or a member read in `await_ready`. On these
    /// @c awaitReadyIsConstantFalse asserts NOTHING, because they reject even a constant `false`:
    /// MSVC before 19.51 -- 19.44 included, the compiler of fastcached#1546 -- and every Clang
    /// before 20, which takes in AppleClang up to 17 (LLVM 19) and emsdk 3.1.56's Clang 19.
    /// `scripts/check-await-ready.py` is what covers those.
#if (defined(__clang__) && __clang_major__ < 20) \
    || (defined(_MSC_VER) && !defined(__clang__) && _MSC_VER < 1951)
    inline constexpr bool CanAskAwaitReadyAtCompileTime = false;
#else
    inline constexpr bool CanAskAwaitReadyAtCompileTime = true;
#endif
} // namespace detail

/// Whether @p A's `await_ready` answers a constant `false`, asked at compile time.
///
/// For a `static_assert` beside an awaiter whose `await_ready` must stay trivial: MSVC 19.44's
/// ARM64 code generator drops the enclosing `try` of a `co_await` on a temporary awaiter whose
/// `await_ready` makes a call
/// ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)). A call, or
/// a read of a member, is not a constant expression, so the assertion then fails to compile.
/// `await_ready` stays a `const` member rather than a `static` one because a static one is
/// reported by clang-tidy's `readability-static-accessed-through-instance` at every `co_await`,
/// in every caller's code. On a compiler that cannot evaluate the question
/// (@c detail::CanAskAwaitReadyAtCompileTime) the answer is `true` and asserts nothing.
/// @tparam A The awaiter type; it need not be constructible in a constant expression.
/// @return False only where `await_ready` is evaluable and does not answer a constant `false`,
///         which is a compile error rather than a value.
template <typename A>
[[nodiscard]] constexpr bool awaitReadyIsConstantFalse() noexcept
{
    return !detail::CanAskAwaitReadyAtCompileTime || !detail::unconstructedAwaiter<A>.await_ready();
}

} // namespace core::async
