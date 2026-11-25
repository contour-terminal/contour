// SPDX-License-Identifier: Apache-2.0
#include <vtpty/StreamKeeper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;
using vtpty::CapturedStream;
using vtpty::OutputExtent;
using vtpty::StreamKeeper;

namespace
{

/// How a test hands its bytes to the keeper: one read cuts no line, a read per byte cuts every one.
enum class Reads : uint8_t
{
    Whole,  ///< One read.
    Bytes,  ///< A read per byte.
    Threes, ///< A read per three bytes.
};

[[nodiscard]] CapturedStream keptOf(std::size_t cap,
                                    std::vector<std::string> prefixes,
                                    std::string_view text,
                                    Reads reads)
{
    auto keeper = StreamKeeper { cap, std::move(prefixes) };
    auto const step = reads == Reads::Whole   ? std::max<std::size_t>(text.size(), 1)
                      : reads == Reads::Bytes ? std::size_t { 1 }
                                              : std::size_t { 3 };
    for (auto const read: std::views::iota(std::size_t { 0 }, (text.size() + step - 1) / step))
        keeper.take(text.substr(read * step, step));
    return std::move(keeper).finish();
}

/// The prefixes the orchestrator drops from `git ls-files -v --stage`: a plain file and a symbolic link.
[[nodiscard]] std::vector<std::string> indexPrefixes()
{
    return { "H 10", "H 12" };
}

struct Row
{
    std::string_view name;
    std::size_t cap;
    std::string_view text;
    std::string_view kept;
    OutputExtent extent;
};

constexpr auto Cap = std::size_t { 1024 };

constexpr auto Rows = std::array {
    Row { .name = "nothing written", .cap = Cap, .text = "", .kept = "", .extent = OutputExtent::Whole },
    Row { .name = "every line dropped",
          .cap = Cap,
          .text = "H 100644 a 0\tx\nH 100755 b 0\ty\nH 120000 c 0\tz\n",
          .kept = "",
          .extent = OutputExtent::Whole },
    Row { .name = "only the flagged lines kept",
          .cap = Cap,
          .text = "H 100644 a 0\tx\nh 100644 b 0\ty\nH 160000 c 0\tsub\nS 100644 d 0\tz\nH 100644 e 0\tw\n",
          .kept = "h 100644 b 0\ty\nH 160000 c 0\tsub\nS 100644 d 0\tz\n",
          .extent = OutputExtent::Whole },
    Row { .name = "a line shorter than a prefix",
          .cap = Cap,
          .text = "H\nH 1\n\n",
          .kept = "H\nH 1\n\n",
          .extent = OutputExtent::Whole },
    Row { .name = "a line that is a prefix and nothing more",
          .cap = Cap,
          .text = "H 10\nH 12",
          .kept = "",
          .extent = OutputExtent::Whole },
    Row { .name = "a last line without a line end, kept",
          .cap = Cap,
          .text = "H 100644 a 0\tx\nH 1",
          .kept = "H 1",
          .extent = OutputExtent::Whole },
    Row { .name = "a prefix in the middle of a line is no prefix",
          .cap = Cap,
          .text = "x H 100644\n",
          .kept = "x H 100644\n",
          .extent = OutputExtent::Whole },
    Row { .name = "the cap counts only what is kept",
          .cap = 4,
          .text = "H 100644 a 0\tx\nh 1\nS 2\n",
          .kept = "h 1\n",
          .extent = OutputExtent::Truncated },
    Row { .name = "a cap of nothing keeps nothing of a kept line",
          .cap = 0,
          .text = "S 1\n",
          .kept = "",
          .extent = OutputExtent::Truncated },
    Row { .name = "a cap of nothing is not exceeded by dropped lines",
          .cap = 0,
          .text = "H 100644 a 0\tx\n",
          .kept = "",
          .extent = OutputExtent::Whole },
};

} // namespace

TEST_CASE("StreamKeeper drops the lines that begin with a declared prefix, however the reads cut them",
          "[vtpty][StreamKeeper]")
{
    for (auto const& row: Rows)
        for (auto const reads: { Reads::Whole, Reads::Bytes, Reads::Threes })
        {
            CAPTURE(row.name, reads);
            auto const kept = keptOf(row.cap, indexPrefixes(), row.text, reads);
            CHECK(kept.bytes == row.kept);
            CHECK(kept.extent == row.extent);
        }
}

TEST_CASE("StreamKeeper without prefixes keeps up to its cap", "[vtpty][StreamKeeper]")
{
    CHECK(keptOf(5, {}, "H 100644\n", Reads::Whole).bytes == "H 100");
    CHECK(keptOf(5, {}, "H 100644\n", Reads::Whole).extent == OutputExtent::Truncated);
    CHECK(keptOf(64, {}, "H 100644\n", Reads::Bytes).bytes == "H 100644\n");
    CHECK(keptOf(64, {}, "H 100644\n", Reads::Bytes).extent == OutputExtent::Whole);
    CHECK(keptOf(0, {}, "", Reads::Whole).extent == OutputExtent::Whole);
}

TEST_CASE("StreamKeeper judges a line in at most the prefixes times the longest of them",
          "[vtpty][StreamKeeper]")
{
    // The most a manifest allows: 16 prefixes of 64 bytes, each a run of 'a' ended by a byte of its own, so
    // that every line of 'a's keeps all 16 live for 63 bytes and is then kept.
    constexpr auto Prefixes = std::size_t { 16 };
    constexpr auto PrefixBytes = std::size_t { 64 };
    auto prefixes = std::vector<std::string> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, Prefixes))
        prefixes.push_back(std::string(PrefixBytes - 1, 'a') + static_cast<char>('b' + index));
    constexpr auto Lines = std::size_t { 64 };
    constexpr auto LineBytes = std::size_t { 64 } * 1024;
    auto const line = std::string(LineBytes - 1, 'a') + '\n';

    auto keeper = StreamKeeper { Lines * LineBytes, std::move(prefixes) };
    for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, Lines))
        for (auto const read: std::views::iota(std::size_t { 0 }, LineBytes / 4096))
            keeper.take(std::string_view(line).substr(read * 4096, 4096));
    CHECK(keeper.comparisons() <= Lines * Prefixes * PrefixBytes);
    auto const kept = std::move(keeper).finish();
    CHECK(kept.bytes.size() == Lines * LineBytes);
    CHECK(kept.extent == OutputExtent::Whole);
}

TEST_CASE("StreamKeeper keeps a large index listing's flagged lines whole under a small cap",
          "[vtpty][StreamKeeper]")
{
    // A listing of a repository with 30000 tracked files is larger than a mebibyte; with the plain entries
    // dropped what is left fits any cap.
    constexpr auto Files = 30'000;
    auto listing = std::string {};
    for (auto const index: std::views::iota(0, Files))
    {
        listing += std::format(
            "H 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\tdir{}/file-{}.txt\n", index % 100, index);
        if (index == Files / 2)
            listing += "h 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\tdir/flagged.txt\n";
    }
    REQUIRE(listing.size() > std::size_t { 1 } << 20);
    auto const kept = keptOf(Cap, indexPrefixes(), listing, Reads::Threes);
    CHECK(kept.bytes == "h 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\tdir/flagged.txt\n");
    CHECK(kept.extent == OutputExtent::Whole);
}
