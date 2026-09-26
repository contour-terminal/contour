// SPDX-License-Identifier: Apache-2.0
#include <core/Ranges.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <version>

using core::findOrNull;
using core::ranges::detail::FoldLeftFallback;
using core::ranges::detail::IotaFallback;

namespace
{
struct Row
{
    int key;
    std::string_view name;
};

constexpr auto Table = std::to_array<Row>({
    Row { .key = 1, .name = "one" },
    Row { .key = 2, .name = "two" },
    Row { .key = 3, .name = "two" }, // duplicate name on purpose; see "first match"
});
} // namespace

TEST_CASE("findOrNull: locates an element by projection", "[base][ranges]")
{
    auto const* const row = findOrNull(Table, 2, &Row::key);
    REQUIRE(row != nullptr);
    REQUIRE(row->name == "two");
}

TEST_CASE("findOrNull: yields nullptr when nothing matches", "[base][ranges]")
{
    // The whole point of the pointer return: "absent" has an idiomatic spelling
    // that does not require naming an iterator type.
    REQUIRE(findOrNull(Table, 99, &Row::key) == nullptr);
    REQUIRE(findOrNull(Table, std::string_view { "nope" }, &Row::name) == nullptr);
}

TEST_CASE("findOrNull: returns the first match, like std::ranges::find", "[base][ranges]")
{
    auto const* const row = findOrNull(Table, std::string_view { "two" }, &Row::name);
    REQUIRE(row != nullptr);
    REQUIRE(row->key == 2);
}

TEST_CASE("findOrNull: points into the range rather than at a copy", "[base][ranges]")
{
    // A copy would make the result useless for the lookup-then-read pattern the
    // helper exists for, and would dangle for a borrowed range.
    auto const* const row = findOrNull(Table, 3, &Row::key);
    REQUIRE(row == &Table[2]);
}

TEST_CASE("findOrNull: works without a projection", "[base][ranges]")
{
    constexpr auto Numbers = std::to_array({ 10, 20, 30 });
    REQUIRE(findOrNull(Numbers, 20) == &Numbers[1]);
    REQUIRE(findOrNull(Numbers, 40) == nullptr);
}

TEST_CASE("findOrNull: handles containers whose iterator is a class type", "[base][ranges]")
{
    // std::array is the case that motivated the helper (raw-pointer iterator on
    // libc++, class type on the MSVC STL), but it must not be array-specific.
    std::vector<Row> const rows { Row { .key = 7, .name = "seven" } };
    auto const* const row = findOrNull(rows, 7, &Row::key);
    REQUIRE(row != nullptr);
    REQUIRE(row->name == "seven");

    std::vector<Row> const empty;
    REQUIRE(findOrNull(empty, 7, &Row::key) == nullptr);
}

TEST_CASE("findOrNull: is usable in a constant expression", "[base][ranges]")
{
    // constexpr matters because a lookup in a constexpr table (fastcached's
    // TraitsOf) would otherwise silently become runtime.
    // The found case is asserted by dereferencing rather than by comparing
    // against nullptr: GCC constant-folds the call, sees the address of a known
    // array element, and rejects `!= nullptr` under -Waddress as a comparison
    // that can never be false. Dereferencing proves non-null just as well —
    // a null dereference is not a constant expression.
    static_assert(findOrNull(Table, 1, &Row::key)->key == 1);
    static_assert(findOrNull(Table, 3, &Row::key)->name == "two");
    static_assert(findOrNull(Table, 42, &Row::key) == nullptr);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// `core::ranges::Iota` and `core::ranges::FoldLeft`.
//
// The fallbacks are called through `detail` directly, so every case below runs on every
// platform whether or not the fallback is SELECTED there. On libstdc++ and the MSVC STL it is
// selected nowhere, and a fallback tested only where it is selected is tested only on the leg
// nobody runs locally -- the leg whose break this seam exists for.
//
// The two agreement cases are compiled only where the standard facility exists, because there
// is nothing to agree with elsewhere. They are absent on such a platform rather than skipped:
// the fallback's own cases above them still run there.

TEST_CASE("Iota fallback: writes successive values across a range and says where it stopped",
          "[base][ranges]")
{
    auto order = std::vector<std::size_t>(5);
    auto const result = IotaFallback(order, std::size_t { 3 });
    CHECK(order == std::vector<std::size_t> { 3, 4, 5, 6, 7 });
    CHECK(result.out == order.end());
    CHECK(result.value == 8);
}

TEST_CASE("Iota fallback: stops at a sentinel of another type", "[base][ranges]")
{
    // A counted iterator against `std::default_sentinel`: iterator and sentinel differ in type,
    // which the range overload over a vector never exercises.
    auto values = std::vector<int>(5, -1);
    auto const result = IotaFallback(std::counted_iterator(values.begin(), 3), std::default_sentinel, 10);
    CHECK(values == std::vector<int> { 10, 11, 12, -1, -1 });
    CHECK(result.out.base() == values.begin() + 3);
    CHECK(result.value == 13);
}

TEST_CASE("Iota fallback: leaves an empty range alone and hands its value back", "[base][ranges]")
{
    auto empty = std::vector<int> {};
    auto const result = IotaFallback(empty, 42);
    CHECK(result.out == empty.end());
    CHECK(result.value == 42);
}

TEST_CASE("FoldLeft fallback: folds from the left in element order", "[base][ranges]")
{
    // ((10 - 1) - 2) - 3 is 4. A right fold gives 1 - (2 - (3 - 10)), which is -8, and every
    // other order differs too -- subtraction and concatenation are the operations that notice.
    auto const numbers = std::vector<int> { 1, 2, 3 };
    CHECK(FoldLeftFallback(numbers, 10, std::minus {}) == 4);

    auto const words = std::vector<std::string> { "a", "b", "c" };
    CHECK(FoldLeftFallback(words, std::string { ">" }, std::plus {}) == ">abc");
}

TEST_CASE("FoldLeft fallback: folds an iterator and a sentinel of another type", "[base][ranges]")
{
    auto const numbers = std::vector<int> { 1, 2, 3, 100 };
    CHECK(FoldLeftFallback(std::counted_iterator(numbers.begin(), 3), std::default_sentinel, 0, std::plus {})
          == 6);
}

TEST_CASE("FoldLeft fallback: answers in the operation's type when there is nothing to fold",
          "[base][ranges]")
{
    // The result type is what the OPERATION returns, decayed -- not the seed's type. With an
    // `int` seed and a `double` operation, an empty range still answers a `double`.
    auto const halve = [](double accumulated, int element) {
        return (accumulated + element) / 2;
    };

    auto const empty = std::vector<int> {};
    auto const nothing = FoldLeftFallback(empty, 7, halve);
    STATIC_REQUIRE(std::same_as<decltype(nothing), double const>);
    CHECK(nothing == 7.0);

    // And a non-empty range, where a result held as the seed's `int` would truncate 4.5 to 4.
    auto const one = std::vector<int> { 2 };
    CHECK(FoldLeftFallback(one, 7, halve) == 4.5);
}

TEST_CASE("FoldLeft fallback: reads a single-pass range whose iterator cannot be copied", "[base][ranges]")
{
    // An istream view's iterator is move-only and single-pass, so a fallback that copied it does
    // not compile and one that read an element twice does not add up.
    auto in = std::istringstream { "1 2 3 4" };
    CHECK(FoldLeftFallback(std::views::istream<int>(in), 0, std::plus {}) == 10);
}

TEST_CASE("Iota and FoldLeft fallbacks: are usable in a constant expression", "[base][ranges]")
{
    // The standard facilities are constexpr, so a fallback that were not would turn a constexpr
    // caller into a build failure on exactly the leg that selects the fallback.
    STATIC_REQUIRE(FoldLeftFallback(std::to_array({ 1, 2, 3 }), 10, std::minus {}) == 4);
    STATIC_REQUIRE([] {
        auto values = std::array<int, 3> {};
        IotaFallback(values, 5);
        return values == std::array { 5, 6, 7 };
    }());
}

#ifdef __cpp_lib_ranges_iota
TEST_CASE("Iota fallback: agrees with the standard facility", "[base][ranges]")
{
    auto viaFallback = std::vector<long>(6);
    auto viaStandard = std::vector<long>(6);
    auto const fallback = IotaFallback(viaFallback, -2L);
    auto const standard = std::ranges::iota(viaStandard, -2L);
    CHECK(viaFallback == viaStandard);
    CHECK(fallback.value == standard.value);
    CHECK(fallback.out - viaFallback.begin() == standard.out - viaStandard.begin());
}
#endif

#ifdef __cpp_lib_ranges_fold
TEST_CASE("FoldLeft fallback: agrees with the standard facility", "[base][ranges]")
{
    auto const halve = [](double accumulated, int element) {
        return (accumulated + element) / 2;
    };
    auto const numbers = std::vector<int> { 4, 8, 15, 16, 23, 42 };
    auto const empty = std::vector<int> {};

    CHECK(FoldLeftFallback(numbers, 100, std::minus {})
          == std::ranges::fold_left(numbers, 100, std::minus {}));
    CHECK(FoldLeftFallback(numbers, 7, halve) == std::ranges::fold_left(numbers, 7, halve));
    CHECK(FoldLeftFallback(empty, 7, halve) == std::ranges::fold_left(empty, 7, halve));
    STATIC_REQUIRE(std::same_as<decltype(FoldLeftFallback(empty, 7, halve)),
                                decltype(std::ranges::fold_left(empty, 7, halve))>);
}
#endif

TEST_CASE("Ranges seam: is the standard facility exactly where its feature-test macro is defined",
          "[base][ranges]")
{
    // By TYPE: the seam holds a copy of the function object rather than a reference to it (the
    // header says why), so the type is what makes it the standard facility -- the same overload
    // set, the same constraints.
#if defined(__cpp_lib_ranges_iota) && !defined(CORE_RANGES_FORCE_FALLBACK)
    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(core::ranges::Iota)>,
                                std::remove_cvref_t<decltype(std::ranges::iota)>>);
#else
    STATIC_REQUIRE(
        std::same_as<std::remove_cvref_t<decltype(core::ranges::Iota)>, core::ranges::detail::IotaFunction>);
#endif

#if defined(__cpp_lib_ranges_fold) && !defined(CORE_RANGES_FORCE_FALLBACK)
    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(core::ranges::FoldLeft)>,
                                std::remove_cvref_t<decltype(std::ranges::fold_left)>>);
#else
    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(core::ranges::FoldLeft)>,
                                core::ranges::detail::FoldLeftFunction>);
#endif
}

TEST_CASE("Ranges seam: answers the calls this tree makes through it", "[base][ranges]")
{
    // The shapes the seam was introduced for, spelled as their call sites spell them, through
    // whichever implementation this build selected.
    auto order = std::vector<std::size_t>(4);
    core::ranges::Iota(order, std::size_t { 0 });
    CHECK(order == std::vector<std::size_t> { 0, 1, 2, 3 });

    auto shuffled = std::vector<int>(3);
    core::ranges::Iota(shuffled, 0);
    CHECK(shuffled == std::vector<int> { 0, 1, 2 });

    auto const swept = std::array<std::size_t, 3> { 2, 0, 5 };
    CHECK(core::ranges::FoldLeft(swept, std::size_t { 0 }, std::plus {}) == 7);
}
