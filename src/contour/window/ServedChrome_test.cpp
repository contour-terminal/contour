// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ServedChrome.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using contour::window::ServedChrome;
using contour::window::servedChromeOf;
using contour::window::ServedMark;
using contour::window::TabMarker;

namespace
{

/// The orchestrator's mark, as the daemon sends it.
[[nodiscard]] ServedMark orchestrator()
{
    return ServedMark { .extension = "orchestrator",
                        .title = "Agents",
                        .moduleDigest = "3f9a0c1d2e4b5a6978e1c0ffee00112233445566778899aabbccddeeff001122" };
}

} // namespace

TEST_CASE("a served tab is marked", "[window][served]")
{
    CHECK(servedChromeOf(orchestrator()).marker == TabMarker::Served);
}

TEST_CASE("a program titling its own tab ◈ Agents gets no marker", "[window][served]")
{
    // The title never reaches the decision: only the daemon's mark does, and a program's tab has none.
    CHECK(servedChromeOf(std::nullopt) == ServedChrome {});
    CHECK(servedChromeOf(std::nullopt).marker == TabMarker::None);
}

TEST_CASE("the served strip's parts: the title, the extension and the module's first 12 hex digits",
          "[window][served]")
{
    CHECK(servedChromeOf(orchestrator())
          == ServedChrome { .marker = TabMarker::Served,
                            .title = "Agents",
                            .extension = "orchestrator",
                            .digest = "3f9a0c1d2e4b" });
}

TEST_CASE("the served strip falls back to the extension's name, and a short or unknown digest stays as it is",
          "[window][served]")
{
    CHECK(
        servedChromeOf(ServedMark { .extension = "stub", .title = "", .moduleDigest = "" })
        == ServedChrome { .marker = TabMarker::Served, .title = "stub", .extension = "stub", .digest = "" });
    CHECK(servedChromeOf(ServedMark { .extension = "stub", .title = "Stub", .moduleDigest = "3f9a" }).digest
          == "3f9a");
}
