// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <vthost/StdioPreamble.hpp>

using vthost::PreambleIncomplete;
using vthost::PreambleOverflow;
using vthost::PreambleReady;
using vthost::PreambleRefused;
using vthost::PreambleScan;
using vthost::PreambleTrailing;
using vthost::scanPreamble;
using vthost::StdioPreambleScanLimit;

namespace
{

/// One input of the scanner and what it must find.
struct ScanRow
{
    std::string name;     ///< What the row is about, for the report.
    std::string received; ///< Everything received so far.
    PreambleScan expected;
};

/// Compares two scan results alternative and fields alike.
[[nodiscard]] bool sameScan(PreambleScan const& actual, PreambleScan const& expected)
{
    if (actual.index() != expected.index())
        return false;
    if (auto const* ready = std::get_if<PreambleReady>(&actual))
        return ready->skipped == std::get<PreambleReady>(expected).skipped;
    if (auto const* refused = std::get_if<PreambleRefused>(&actual))
        return refused->message == std::get<PreambleRefused>(expected).message
               && refused->skipped == std::get<PreambleRefused>(expected).skipped;
    if (auto const* trailing = std::get_if<PreambleTrailing>(&actual))
        return trailing->extra == std::get<PreambleTrailing>(expected).extra;
    return true;
}

[[nodiscard]] std::vector<ScanRow> scanRows()
{
    auto const junk = std::string(StdioPreambleScanLimit + 1, 'x');
    return {
        { "nothing yet", "", PreambleIncomplete {} },
        { "half a ready line", "contour-stdio 1 rea", PreambleIncomplete {} },
        { "the ready line", "contour-stdio 1 ready\n", PreambleReady { .skipped = 0 } },
        { "a banner first", "Welcome to homepc\ncontour-stdio 1 ready\n", PreambleReady { .skipped = 18 } },
        { "the error line",
          "contour-stdio 1 error: daemon did not start within 5 seconds.\n",
          PreambleRefused { .message = "daemon did not start within 5 seconds.", .skipped = 0 } },
        { "half an error line", "contour-stdio 1 error: daemon did", PreambleIncomplete {} },
        { "a byte after the ready line", "contour-stdio 1 ready\nX", PreambleTrailing { .extra = 1 } },
        { "too much junk", junk, PreambleOverflow {} },
        { "too much junk, then a partial preamble", junk + "contour-st", PreambleOverflow {} },
        { "as much junk as allowed, then a partial preamble",
          std::string(StdioPreambleScanLimit, 'x') + "contour-st",
          PreambleIncomplete {} },
        { "as much junk as allowed, then the ready line",
          std::string(StdioPreambleScanLimit, 'x') + "contour-stdio 1 ready\n",
          PreambleReady { .skipped = StdioPreambleScanLimit } },
    };
}

} // namespace

TEST_CASE("scanPreamble finds the stdio preamble", "[vthost][stdio]")
{
    for (auto const& row: scanRows())
    {
        INFO(row.name);
        CHECK(sameScan(scanPreamble(row.received), row.expected));
    }
}

TEST_CASE("stdioPreambleError keeps its message on one line", "[vthost][stdio]")
{
    CHECK(vthost::stdioPreambleError("a\nb") == "contour-stdio 1 error: a b\n");
    CHECK(vthost::stdioPreambleError("a\r\nb") == "contour-stdio 1 error: a  b\n");
    CHECK(sameScan(scanPreamble(vthost::stdioPreambleError("why")),
                   PreambleRefused { .message = "why", .skipped = 0 }));
}
