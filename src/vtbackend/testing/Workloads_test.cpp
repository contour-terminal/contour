// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/Workloads.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <set>
#include <string>
#include <string_view>

using namespace std::string_view_literals;
using vtbackend::test::makeWorkload;
using vtbackend::test::Workload;
using vtbackend::test::Workloads;

namespace
{

constexpr auto TargetBytes = size_t { 64 } * 1024;

/// What a workload's output must contain, so that a generator cannot quietly stop exercising its path.
struct Signature
{
    std::string_view workload;
    std::string_view mustContain;
};

constexpr auto Signatures = std::array {
    Signature { .workload = "ascii", .mustContain = "\r\n" },
    Signature { .workload = "colored", .mustContain = "\033[1;32m" },
    Signature { .workload = "sgr", .mustContain = "\033[38;5;" },
    Signature { .workload = "utf8", .mustContain = "\xC3\xA4" },
    Signature { .workload = "tui", .mustContain = "\033[50;1H" },
};

/// What a workload generates at TargetBytes: its size, and the FNV-1a digest of its bytes.
struct Golden
{
    std::string_view workload;
    size_t size;
    uint64_t digest;
};

/// The bytes every benchmark baseline was measured on. A generator change that alters them makes every
/// earlier measurement incomparable, so it must show up here first, and come with a new baseline.
constexpr auto Goldens = std::array {
    Golden { .workload = "ascii", .size = 65554, .digest = 0x5a83029e1652730a },
    Golden { .workload = "colored", .size = 65593, .digest = 0x1dee961d0676b761 },
    Golden { .workload = "sgr", .size = 65582, .digest = 0xf1068e82e13a4813 },
    Golden { .workload = "utf8", .size = 65545, .digest = 0x417b51eebafc7b58 },
    Golden { .workload = "tui", .size = 65812, .digest = 0x5fc962bd2d35cc14 },
};

/// @return The 64-bit FNV-1a digest of @p bytes.
constexpr uint64_t fnv1a(std::string_view bytes)
{
    auto digest = uint64_t { 0xcbf29ce484222325 };
    for (auto const byte: bytes)
    {
        digest ^= static_cast<unsigned char>(byte);
        digest *= 0x100000001b3;
    }
    return digest;
}
static_assert(fnv1a("") == 0xcbf29ce484222325 && fnv1a("a") == 0xaf63dc4c8601ec8c); // the reference vectors

Workload const& workloadNamed(std::string_view name)
{
    auto const workload = std::ranges::find(Workloads, name, &Workload::name);
    REQUIRE(workload != Workloads.end());
    return *workload;
}

} // namespace

TEST_CASE("Workloads.names_are_unique", "[workloads]")
{
    auto names = std::set<std::string_view> {};
    for (auto const& workload: Workloads)
        CHECK(names.insert(workload.name).second);
}

TEST_CASE("Workloads.are_deterministic", "[workloads]")
{
    for (auto const& workload: Workloads)
    {
        INFO(workload.name);
        CHECK(makeWorkload(workload, TargetBytes) == makeWorkload(workload, TargetBytes));
    }
}

TEST_CASE("Workloads.bytes_are_pinned", "[workloads]")
{
    CHECK(Goldens.size() == Workloads.size());
    for (auto const& golden: Goldens)
    {
        INFO(golden.workload);
        auto const text = makeWorkload(workloadNamed(golden.workload), TargetBytes);
        CHECK(text.size() == golden.size);
        CHECK(fnv1a(text) == golden.digest);
    }
}

TEST_CASE("Workloads.reach_the_target_size_in_whole_units", "[workloads]")
{
    for (auto const& workload: Workloads)
    {
        INFO(workload.name);
        auto const text = makeWorkload(workload, TargetBytes);
        CHECK(text.size() >= TargetBytes);
        CHECK(text.size() < TargetBytes + (size_t { 16 } * 1024));
        // Lines end in CRLF, frames in an erase to the end of their last row.
        CHECK((text.ends_with("\r\n") || text.ends_with("\033[K")));
    }
}

TEST_CASE("Workloads.contain_what_they_stand_for", "[workloads]")
{
    for (auto const& signature: Signatures)
    {
        INFO(signature.workload);
        CHECK(makeWorkload(workloadNamed(signature.workload), TargetBytes).contains(signature.mustContain));
    }
}

TEST_CASE("Workloads.ascii_is_printable_text_and_line_ends", "[workloads]")
{
    auto const text = makeWorkload(workloadNamed("ascii"), TargetBytes);
    CHECK(std::ranges::all_of(
        text, [](char ch) { return (ch >= 0x21 && ch <= 0x7E) || ch == '\r' || ch == '\n'; }));
}

TEST_CASE("Workloads.utf8_holds_no_byte_above_0x7F_but_U+00E4", "[workloads]")
{
    auto const text = makeWorkload(workloadNamed("utf8"), TargetBytes);
    auto rest = std::string {};
    for (auto const part: std::views::split(text, "\xC3\xA4"sv))
        rest.append(part.begin(), part.end());
    CHECK(rest.size() < text.size());
    CHECK(std::ranges::none_of(rest, [](char ch) { return static_cast<unsigned char>(ch) >= 0x80; }));
}

TEST_CASE("Workloads.tui_repaints_every_row_without_a_line_feed", "[workloads]")
{
    auto const text = makeWorkload(workloadNamed("tui"), TargetBytes);
    CHECK(text.starts_with("\033[1;1H"));
    CHECK(!text.contains('\n'));
    for (auto const row: std::views::iota(1, vtbackend::test::WorkloadPageLines + 1))
        CHECK(text.contains(std::format("\033[{};1H", row)));
}
