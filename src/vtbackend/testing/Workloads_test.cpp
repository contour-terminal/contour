// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/Workloads.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
