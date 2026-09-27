// SPDX-License-Identifier: Apache-2.0
#include <core/Generator.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

using core::Generator;
using core::detail::GeneratorFallback;

// Generator.hpp is included first, before anything could have defined __cpp_lib_generator for
// it. Where the standard library has <generator> (and is not libstdc++, see Generator.hpp),
// Generator must be std::generator all the same: the choice may not depend on what a translation
// unit happened to include first, or two translation units disagree about a virtual function's
// return type (FileSystem's walk).
#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L && !defined(__GLIBCXX__) \
    && !defined(CORE_GENERATOR_FORCE_FALLBACK)
    #include <generator>
static_assert(std::is_same_v<Generator<int>, std::generator<int>>,
              "the standard library has std::generator, so Generator must be it");
#else
static_assert(std::is_same_v<Generator<int>, GeneratorFallback<int>>);
#endif
static_assert(std::ranges::input_range<GeneratorFallback<int>>);
static_assert(std::ranges::input_range<Generator<int>>);

namespace
{
/// The generator the tests below run under: the one this platform's Generator is, and the
/// fallback, which some platforms (libc++, and so WebAssembly and macOS) use as their Generator
/// and which is therefore tested everywhere.
struct UseGenerator
{
    template <typename T>
    using Type = Generator<T>;
};

struct UseFallback
{
    template <typename T>
    using Type = GeneratorFallback<T>;
};

/// A coroutine yielding the integers [0, n).
template <template <typename> typename G>
G<int> countTo(int n)
{
    for (auto const i: std::views::iota(0, n))
        co_yield i;
}

// Note: exception propagation out of a generator is intentionally NOT tested
// here. `Generator` correctly captures a coroutine-body exception and rethrows
// it to the consumer (verified standalone), but throwing *through a coroutine
// frame* crashes the Catch2 test harness on Windows (an MSVC coroutine-unwind /
// vectored-exception-handler interaction that also affects std::generator). It
// is moot for production: every Generator in core-cpp is driven by non-throwing
// iteration (the error_code recursive_directory_iterator overload), so no
// exception ever propagates out of a generator.

/// A coroutine yielding owning strings (verifies non-trivial value types).
template <template <typename> typename G>
G<std::string> words()
{
    co_yield std::string("alpha");
    co_yield std::string("beta");
}

template <template <typename> typename G>
G<int> infinite()
{
    for (auto const i: std::views::iota(0))
        co_yield i;
}
} // namespace

TEMPLATE_TEST_CASE("Generator yields every value in order", "[Generator]", UseGenerator, UseFallback)
{
    auto collected = std::vector<int> {};
    for (auto const v: countTo<TestType::template Type>(4))
        collected.push_back(v);

    REQUIRE(collected == std::vector<int> { 0, 1, 2, 3 });
}

TEMPLATE_TEST_CASE("Generator over an empty sequence yields nothing",
                   "[Generator]",
                   UseGenerator,
                   UseFallback)
{
    auto count = 0;
    for ([[maybe_unused]] auto const v: countTo<TestType::template Type>(0))
        ++count;

    REQUIRE(count == 0);
}

TEMPLATE_TEST_CASE("Generator yields owning values", "[Generator]", UseGenerator, UseFallback)
{
    auto collected = std::vector<std::string> {};
    for (auto const& w: words<TestType::template Type>())
        collected.push_back(w);

    REQUIRE(collected == std::vector<std::string> { "alpha", "beta" });
}

TEMPLATE_TEST_CASE("Generator is move-only and the moved-from frame is not double-freed",
                   "[Generator]",
                   UseGenerator,
                   UseFallback)
{
    STATIC_CHECK(!std::is_copy_constructible_v<typename TestType::template Type<int>>);

    auto gen = countTo<TestType::template Type>(3);
    auto moved = std::move(gen);

    auto collected = std::vector<int> {};
    for (auto const v: moved)
        collected.push_back(v);

    REQUIRE(collected == std::vector<int> { 0, 1, 2 });
}

TEMPLATE_TEST_CASE("Breaking out of a Generator destroys the suspended frame",
                   "[Generator]",
                   UseGenerator,
                   UseFallback)
{
    // An infinite generator that we abandon after a few elements must not hang
    // and must release its frame (verified for leaks under ASAN).
    auto seen = 0;
    for ([[maybe_unused]] auto const v: infinite<TestType::template Type>())
    {
        if (++seen == 5)
            break;
    }
    REQUIRE(seen == 5);
}
