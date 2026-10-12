// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <random>
#include <ranges>
#include <string>
#include <string_view>

/// Synthetic terminal output for the benchmarks: what one kind of application writes, generated from a fixed
/// seed so that every run on every machine parses the same bytes.
namespace vtbackend::test
{

/// The page height the workloads assume: the TUI workload repaints this many rows per frame.
constexpr auto WorkloadPageLines = 50;

/// The page width the workloads assume: wider than the longest line any of them writes (180 columns).
constexpr auto WorkloadPageColumns = 200;

/// The seed every workload is generated from.
constexpr auto WorkloadSeed = 42u;

/// Appends one unit of output -- a line, or a full-screen frame -- the way one kind of application writes it.
/// @param out Where the output goes.
/// @param rng The run's random source; the seed fixes the whole output.
using WorkloadWriter = void (*)(std::string& out, std::mt19937& rng);

/// One synthetic workload: what one kind of application writes to its terminal.
struct Workload
{
    std::string_view name;        ///< How the command line names it.
    std::string_view description; ///< What it stands for.
    WorkloadWriter writeUnit;     ///< Appends one line or one frame of it.
};

namespace detail
{
    /// Appends @p count random printable ASCII characters (0x21 to 0x7E).
    inline void appendPrintable(std::string& out, std::mt19937& rng, int count)
    {
        auto printable = std::uniform_int_distribution<int> { 0x21, 0x7E };
        for ([[maybe_unused]] auto const column: std::views::iota(0, count))
            out.push_back(static_cast<char>(printable(rng)));
    }

    /// @return A line length as source code or a log file has them: 20 to 180 columns.
    inline int lineLength(std::mt19937& rng)
    {
        return std::uniform_int_distribution<int> { 20, 180 }(rng);
    }

    /// Plain text, as `cat` of a large file writes it.
    inline void writeAsciiLine(std::string& out, std::mt19937& rng)
    {
        appendPrintable(out, rng, lineLength(rng));
        out += "\r\n";
    }

    /// One coloured token at the start of the line, as `ls --color`, a compiler or `git log` write.
    inline void writeColoredLine(std::string& out, std::mt19937& rng)
    {
        auto const length = lineLength(rng);
        out += "\033[1;32m";
        appendPrintable(out, rng, 8);
        out += "\033[0m ";
        appendPrintable(out, rng, length - 9);
        out += "\r\n";
    }

    /// A colour change every word, as a syntax highlighter writes.
    inline void writeSgrLine(std::string& out, std::mt19937& rng)
    {
        for (auto const word: std::views::iota(0, lineLength(rng) / 8))
        {
            out += std::format("\033[38;5;{}m", (word * 7) % 256);
            appendPrintable(out, rng, 7);
            out.push_back(' ');
        }
        out += "\033[0m\r\n";
    }

    /// Latin text with every fifth character U+00E4, two bytes in UTF-8.
    inline void writeUtf8Line(std::string& out, std::mt19937& rng)
    {
        for (auto const column: std::views::iota(0, lineLength(rng)))
        {
            if (column % 5 == 0)
                out += "\xC3\xA4";
            else
                appendPrintable(out, rng, 1);
        }
        out += "\r\n";
    }

    /// One full-screen repaint as htop, vim or tmux write it: per row, position, colour, text, erase to the
    /// end.
    inline void writeTuiFrame(std::string& out, std::mt19937& rng)
    {
        auto const shade = std::uniform_int_distribution<int> { 0, 255 }(rng);
        for (auto const row: std::views::iota(1, WorkloadPageLines + 1))
        {
            out += std::format("\033[{};1H\033[38;5;{}m", row, ((row * 7) + shade) % 256);
            appendPrintable(out, rng, 60);
            out += "\033[0m\033[48;5;236m";
            appendPrintable(out, rng, 60);
            out += "\033[0m";
            appendPrintable(out, rng, 60);
            out += "\033[K";
        }
    }
} // namespace detail

/// Every workload the benchmarks know, in the order they report them. A new workload is a new row.
inline constexpr auto Workloads = std::array {
    Workload { .name = "ascii",
               .description = "plain text, as cat of a large file writes it",
               .writeUnit = &detail::writeAsciiLine },
    Workload { .name = "colored",
               .description = "one coloured token per line, as ls --color or a compiler writes",
               .writeUnit = &detail::writeColoredLine },
    Workload { .name = "sgr",
               .description = "a colour change every word, as a syntax highlighter writes",
               .writeUnit = &detail::writeSgrLine },
    Workload { .name = "utf8",
               .description = "Latin text with every fifth character outside ASCII",
               .writeUnit = &detail::writeUtf8Line },
    Workload { .name = "tui",
               .description = "full-screen repaints, as htop, vim or tmux write them",
               .writeUnit = &detail::writeTuiFrame },
};

/// Generates @p workload until it holds at least @p targetBytes, ending on a whole line or frame.
/// @return The same bytes for the same arguments, on every run and every machine.
[[nodiscard]] inline std::string makeWorkload(Workload const& workload, size_t targetBytes)
{
    // Predictable on purpose: one fixed seed, spread over the engine's whole state by the seed_seq algorithm
    // the standard specifies.
    auto seeds = std::seed_seq { WorkloadSeed };
    auto rng = std::mt19937 { seeds };
    auto out = std::string {};
    out.reserve(targetBytes + (size_t { 16 } * 1024));
    while (out.size() < targetBytes)
        workload.writeUnit(out, rng);
    return out;
}

} // namespace vtbackend::test
