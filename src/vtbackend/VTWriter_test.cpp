// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Line.h>
#include <vtbackend/VTWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace vtbackend;

TEST_CASE("VTWriter.writeBlankLine.preservesBoldFromFillAttrs", "[VTWriter][blank]")
{
    // Regression: the blank-line fast path must mirror the per-cell path's Bold handling
    // (SGR 1). Previously it unconditionally emitted GraphicsRendition::Normal (SGR 22),
    // silently dropping bold weight carried in fillAttrs.flags.
    auto attrs = GraphicsAttributes {};
    attrs.flags = CellFlags { CellFlag::Bold };
    auto line = Line(ColumnCount(4), LineFlags {}, attrs);
    REQUIRE(line.isBlank());

    auto output = std::vector<char> {};
    auto writer = VTWriter(output);
    writer.write(line);

    auto const emitted = std::string(output.data(), output.size());
    // SGR params are combined into one prologue (e.g. "\e[1;39;49m    ").
    CHECK(emitted.starts_with("\x1b[1;"));
    // Must NOT emit the Normal weight (SGR 22), which would clear bold.
    CHECK(emitted.find("22") == std::string::npos);
    // Four spaces for the four blank columns follow the SGR prologue.
    CHECK(emitted.find("    ") != std::string::npos);
}

TEST_CASE("VTWriter.writeBlankLine.emitsNormalWhenNoBold", "[VTWriter][blank]")
{
    // Without Bold in fillAttrs, the blank-line path emits Normal (SGR 22) before the
    // spaces to match the per-cell path (which sets Bold/Normal explicitly each cell).
    auto line = Line(ColumnCount(3), LineFlags {}, GraphicsAttributes {});
    REQUIRE(line.isBlank());

    auto output = std::vector<char> {};
    auto writer = VTWriter(output);
    writer.write(line);

    auto const emitted = std::string(output.data(), output.size());
    CHECK(emitted.starts_with("\x1b[22;"));
    CHECK(emitted.find("   ") != std::string::npos);
}
