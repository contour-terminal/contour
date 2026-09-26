// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <ranges>
#include <type_traits>
#include <utility>
#include <version>

namespace core
{

/// Find the first element of @p range whose @p projection compares equal to
/// @p value, yielding a pointer.
///
/// This exists for the **return type**, not the algorithm: `std::ranges::find`
/// is available on every toolchain the project builds with, but the iterator it
/// hands back cannot be named portably. `std::array`'s iterator is a raw pointer
/// in libc++ and a class type in the MSVC STL, so a call site that stores the
/// result has no working spelling — `auto const*` fails to compile on Windows
/// with C3535, a plain `auto const` trips clang-tidy's
/// readability-qualified-auto, and writing the type out trips its
/// modernize-use-auto. Each of those has broken a build here.
///
/// Inside a template the iterator's type is dependent, so the conflict resolves
/// once, here, instead of at every lookup. Callers get a plain pointer, which
/// also gives "not found" its idiomatic spelling:
///
/// ```cpp
/// if (auto const* const row = findOrNull(Table, key, &Row::key))
///     return row->value;
/// ```
///
/// Prefer this over `std::ranges::find` whenever the range may be a
/// `std::array` or a raw array; for `std::vector` and friends the iterator is a
/// class type everywhere and either form is portable.
///
/// @param range Range to search. Borrowed, never copied.
/// @param value Value each projected element is compared against.
/// @param projection Applied to an element before comparing; identity by default.
/// @return Pointer to the first match, or `nullptr` when nothing matches.
template <std::ranges::forward_range Range, typename Value, typename Projection = std::identity>
[[nodiscard]] constexpr std::ranges::range_value_t<Range> const* findOrNull(Range const& range,
                                                                            Value const& value,
                                                                            Projection projection = {})
{
    auto const it = std::ranges::find(range, value, std::move(projection));
    return it != std::ranges::end(range) ? std::addressof(*it) : nullptr;
}

/// Find the first element of @p range satisfying @p predicate, yielding a pointer.
///
/// Like `findOrNull`, this exists for the **return type** rather than the
/// algorithm, and the reason is spelled out here rather than cross-referenced
/// because a reader arriving at the predicate form needs it *here*:
/// `std::ranges::find_if` is available on every toolchain the project builds
/// with, but the iterator it hands back cannot be named portably.
/// `std::array`'s iterator is a raw pointer in libstdc++ and libc++ and a class
/// type in the MSVC STL, so a call site that stores the result has no working
/// spelling — `auto const*` fails to compile on Windows with C3535, a plain
/// `auto const` trips clang-tidy's readability-qualified-auto, and writing the
/// type out trips its modernize-use-auto. Each of those has broken a build here.
///
/// Inside a template the iterator's type is dependent, so the conflict resolves
/// once, here, instead of at every lookup:
///
/// ```cpp
/// if (auto const* const row = findIfOrNull(Table, [&](Row const& r) { return r.needle.contains(x); }))
///     return row->value;
/// ```
///
/// **Use this rather than forcing `findOrNull`** with a `bool`-returning
/// projection and a `true` value. That spelling type-checks, and it inverts the
/// reading of every equality-on-a-member call site `findOrNull` has.
///
/// No projection parameter, unlike `findOrNull`: a predicate already subsumes
/// one, so a second customization point would be a spelling with no question of
/// its own to answer.
///
/// @param range Range to search. Borrowed, never copied.
/// @param predicate Applied to each element; the first `true` wins.
/// @return Pointer to the first match, or `nullptr` when nothing matches.
template <std::ranges::forward_range Range, typename Predicate>
[[nodiscard]] constexpr std::ranges::range_value_t<Range> const* findIfOrNull(Range const& range,
                                                                              Predicate predicate)
{
    auto const it = std::ranges::find_if(range, std::move(predicate));
    return it != std::ranges::end(range) ? std::addressof(*it) : nullptr;
}

} // namespace core

// ---------------------------------------------------------------------------
// C++23 range algorithms that a standard library core-cpp builds with does not ship yet:
// `ranges::Iota` and `ranges::FoldLeft`. The libc++ of emsdk 3.1.56 (17.0.4) defines neither
// feature-test macro, so the WebAssembly subset uses both fallbacks there.
//
// ## Why a seam, and not the older spelling
//
// `std::ranges::iota` and `std::ranges::fold_left` say what the code means, and are worth
// keeping rather than retreating to `std::iota` and `std::accumulate` at every call site. They
// are missing from Apple's own libc++, which fastcached found when `Package (macOS .pkg)`, a
// job that was not a required check, answered `error: no member named 'iota' in namespace
// 'std::ranges'` while libstdc++, the MSVC STL and Homebrew's LLVM libc++ all compiled it
// (https://github.com/LASTRADA-Software/fastcached/pull/1392).
//
// ## How a facility is selected
//
// By the standard feature-test macro from `<version>`, never by a compiler or library
// version: the macro is the library's own statement of what it ships, and a version table
// would be a second source of truth about somebody else's release notes. Where the macro is
// defined, the seam name is an object OF THE STANDARD FUNCTION OBJECT'S TYPE -- the same
// overloads, the same constraints, the same diagnostics, nothing of ours in between. Where it
// is not, it is the implementation below, transcribed from [numeric.iota] and [alg.fold] with
// the standard's constraints and return types.
//
// A defined macro over a library that lacks the facility would be that library's defect. The
// reverse -- a facility present with its macro undefined -- is ORDINARY and harmless, and it
// is not hypothetical. Measured on libc++ 22 (`/usr/lib/llvm-22/include/c++/v1/version`):
// `__cpp_lib_ranges_iota` is defined, `__cpp_lib_ranges_fold` is NOT, although
// `<__algorithm/ranges_fold.h>` implements `fold_left`. Inferred, not measured: the macro
// waits for the whole P2322 family, `fold_right` included. So a libc++ build selects the
// `FoldLeft` fallback even where `std::ranges::fold_left` exists, which costs nothing.
//
// ## Why the fallback is compiled everywhere
//
// `detail::IotaFallback` and `detail::FoldLeftFallback` are compiled on every platform,
// selected or not, and `Ranges_test.cpp` calls them directly and asserts they agree with the
// standard facility wherever one exists. A fallback compiled only where it is selected is
// compiled only on the leg nobody runs locally, which is the leg that broke.
//
// ## `CORE_RANGES_FORCE_FALLBACK`
//
// A TEST-ONLY define selecting every fallback although the standard facility exists, so a
// Linux or Windows build can compile the real call sites against the fallback -- the one
// local stand-in for the leg that lacks the facility. It is WHOLE-BUILD ONLY
// (`CMAKE_CXX_FLAGS`): defined in one translation unit and not in another, `ranges::Iota`
// names two different entities under one name, an ODR violation no toolchain diagnoses.
// Nothing in the build sets it.
//
// ## Naming
//
// CamelCase, `ranges::Iota` rather than `ranges::iota`: the seam objects are constants, which
// `.clang-tidy` spells CamelCase, and a seam spelling that differs from the standard one is
// what lets a case-sensitive scan tell a use of the seam from a direct use. The `ranges`
// namespace keeps the call site reading as a range algorithm.
//
// A COPY of the function object, never a reference to it, and that is the naming rule too.
// `readability-identifier-naming` files a `constexpr auto const&` under global VARIABLES,
// because a reference type is not const-qualified, so it demands `iota` -- measured with
// clang-tidy 22. A copy is a global CONSTANT and takes `Iota`. The copy is sound because every
// library in the matrix defines these objects as empty classes with implicit copies --
// measured in libstdc++ 14 (`__iota_fn`), libc++ 22 (`__iota_fn`) and the MSVC STL 14.44 and
// 14.51 (`_Iota_fn`, `_Fold_left_fn`) -- and a library that ever made one uncopyable fails to
// BUILD here, loudly, rather than misbehaving. `Ranges_test.cpp` asserts the TYPE is the
// standard one wherever it is selected.
namespace core::ranges
{

namespace detail
{
    /// What `IotaFallback` returns: where writing stopped, and the value that would have been
    /// written next.
    ///
    /// The shape of `std::ranges::out_value_result`, which ships under the same feature-test
    /// macro as `std::ranges::iota` and so cannot be borrowed by the fallback that exists for
    /// that macro's absence. Callers read it through `auto`, `.out` / `.value` or a structured
    /// binding, all of which both types answer alike.
    template <typename Out, typename Value>
    struct OutValueResult
    {
        /// One past the last position written.
        Out out;

        /// The value that would have been written next.
        Value value;
    };

    /// `std::ranges::iota` ([numeric.iota]), for a standard library that does not ship it.
    struct IotaFunction
    {
        /// Writes @p value and then each successor of it to every position in
        /// [@p out, @p bound).
        /// @param out Where the first value is written.
        /// @param bound Where writing stops.
        /// @param value The first value written.
        /// @return One past the last position written, and the value that would have been
        ///         written next.
        template <std::input_or_output_iterator Out,
                  std::sentinel_for<Out> Sentinel,
                  std::weakly_incrementable Value>
            requires std::indirectly_writable<Out, Value const&>
        constexpr OutValueResult<Out, Value> operator()(Out out, Sentinel bound, Value value) const
        {
            while (out != bound)
            {
                *out = std::as_const(value);
                ++out;
                ++value;
            }
            return { .out = std::move(out), .value = std::move(value) };
        }

        /// Writes @p value and then each successor of it to every element of @p range.
        /// @param range The range written to. Borrowed; an rvalue that is not a borrowed range
        ///        yields `std::ranges::dangling` in place of an iterator, as the standard does.
        /// @param value The first value written.
        /// @return One past the last element written, and the value that would have been
        ///         written next.
        template <std::weakly_incrementable Value, std::ranges::output_range<Value const&> Range>
        constexpr OutValueResult<std::ranges::borrowed_iterator_t<Range>, Value> operator()(Range&& range,
                                                                                            Value value) const
        {
            // Forwarded into a named reference, which `ranges::begin` and `ranges::end` then read
            // as an lvalue -- exactly what naming `range` directly would do. The standard's own
            // wording names `range`; `cppcoreguidelines-missing-std-forward` refuses a forwarding
            // reference that is never forwarded, and this satisfies it without changing what is read.
            auto&& target = std::forward<Range>(range);
            auto [out, next] =
                (*this)(std::ranges::begin(target), std::ranges::end(target), std::move(value));
            return { .out = std::move(out), .value = std::move(next) };
        }
    };

    /// [alg.fold]'s exposition-only `indirectly-binary-left-foldable-impl`, transcribed.
    template <typename Op, typename Init, typename It, typename Result>
    concept IndirectlyBinaryLeftFoldableImpl =
        std::movable<Init> && std::movable<Result> && std::convertible_to<Init, Result>
        && std::invocable<Op&, Result, std::iter_reference_t<It>>
        && std::assignable_from<Result&, std::invoke_result_t<Op&, Result, std::iter_reference_t<It>>>;

    /// [alg.fold]'s exposition-only `indirectly-binary-left-foldable`, transcribed.
    template <typename Op, typename Init, typename It>
    concept IndirectlyBinaryLeftFoldable =
        std::copy_constructible<Op> && std::indirectly_readable<It>
        && std::invocable<Op&, Init, std::iter_reference_t<It>>
        && std::convertible_to<std::invoke_result_t<Op&, Init, std::iter_reference_t<It>>,
                               std::decay_t<std::invoke_result_t<Op&, Init, std::iter_reference_t<It>>>>
        && IndirectlyBinaryLeftFoldableImpl<
            Op,
            Init,
            It,
            std::decay_t<std::invoke_result_t<Op&, Init, std::iter_reference_t<It>>>>;

    /// `std::ranges::fold_left` ([alg.fold]), for a standard library that does not ship it.
    struct FoldLeftFunction
    {
        /// Folds [@p first, @p last) from the left: `op(op(op(init, e0), e1), e2)`.
        /// @param first The first element.
        /// @param last Where the elements end.
        /// @param init The value folded with the first element.
        /// @param op The binary operation, called as `op(accumulated, element)`.
        /// @return The fold, as the decayed type `op` returns; @p init converted to that type
        ///         when there are no elements.
        template <std::input_iterator It,
                  std::sentinel_for<It> Sentinel,
                  typename Init = std::iter_value_t<It>,
                  IndirectlyBinaryLeftFoldable<Init, It> Op>
        [[nodiscard]] constexpr auto operator()(It first, Sentinel last, Init init, Op op) const
        {
            using Result = std::decay_t<std::invoke_result_t<Op&, Init, std::iter_reference_t<It>>>;
            if (first == last)
                return static_cast<Result>(std::move(init));
            Result accumulated = std::invoke(op, std::move(init), *first);
            ++first;
            while (first != last)
            {
                accumulated = std::invoke(op, std::move(accumulated), *first);
                ++first;
            }
            return accumulated;
        }

        /// Folds @p range from the left: `op(op(op(init, e0), e1), e2)`.
        /// @param range The elements. Read, never retained.
        /// @param init The value folded with the first element.
        /// @param op The binary operation, called as `op(accumulated, element)`.
        /// @return The fold, as the decayed type `op` returns; @p init converted to that type
        ///         when @p range is empty.
        template <std::ranges::input_range Range,
                  typename Init = std::ranges::range_value_t<Range>,
                  IndirectlyBinaryLeftFoldable<Init, std::ranges::iterator_t<Range>> Op>
        [[nodiscard]] constexpr auto operator()(Range&& range, Init init, Op op) const
        {
            // Forwarded into a named reference for the reason `IotaFunction`'s range overload gives.
            auto&& source = std::forward<Range>(range);
            return (*this)(
                std::ranges::begin(source), std::ranges::end(source), std::move(init), std::ref(op));
        }
    };

    /// The fallback for `ranges::Iota`, callable on every platform whether or not it is selected.
    inline constexpr IotaFunction IotaFallback {};

    /// The fallback for `ranges::FoldLeft`, callable on every platform whether or not it is selected.
    inline constexpr FoldLeftFunction FoldLeftFallback {};
} // namespace detail

/// `std::ranges::iota` where the standard library ships it, `detail::IotaFallback` where it does
/// not. Call it exactly as the standard facility: `core::ranges::Iota(order, std::size_t { 0 });`.
#if defined(__cpp_lib_ranges_iota) && !defined(CORE_RANGES_FORCE_FALLBACK)
inline constexpr auto Iota = std::ranges::iota;
#else
inline constexpr auto Iota = detail::IotaFallback;
#endif

/// `std::ranges::fold_left` where the standard library ships it, `detail::FoldLeftFallback` where
/// it does not. Call it exactly as the standard facility:
/// `core::ranges::FoldLeft(counts, std::size_t { 0 }, std::plus {});`.
#if defined(__cpp_lib_ranges_fold) && !defined(CORE_RANGES_FORCE_FALLBACK)
inline constexpr auto FoldLeft = std::ranges::fold_left;
#else
inline constexpr auto FoldLeft = detail::FoldLeftFallback;
#endif

} // namespace core::ranges
