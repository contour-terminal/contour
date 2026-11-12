// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/PresentationState.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("PresentationState.decodeCursorInformation reads every field", "[presentation-state]")
{
    // Line 3, column 7, page 1; bold+inverse; protected; origin mode + wrap pending; GL=G1, GR=G3;
    // G2 holds a 96-set; designators B, 0, A (96: Latin-1) and %5 (one Contour has no map for).
    auto const info = decodeCursorInformation("3;7;1;I;A;I;1;3;D;B0A%5");
    REQUIRE(info.has_value());
    CHECK(info->line == 3);
    CHECK(info->column == 7);
    CHECK(info->page == 1);
    CHECK(info->rendition == 0x09);
    CHECK(info->isProtected);
    CHECK(info->has(CursorInformationFlag::OriginMode));
    CHECK(info->has(CursorInformationFlag::WrapPending));
    CHECK_FALSE(info->has(CursorInformationFlag::SingleShift2));
    CHECK(info->gl == CharsetTable::G1);
    CHECK(info->gr == CharsetTable::G3);
    CHECK(info->is96Charset(CharsetTable::G2));
    CHECK_FALSE(info->is96Charset(CharsetTable::G0));
    CHECK(info->designations == std::array<std::string, 4> { "B", "0", "A", "%5" });
}

TEST_CASE("PresentationState.decodeCursorInformation rejects what it cannot read", "[presentation-state]")
{
    struct Row
    {
        std::string_view payload;
        PresentationStateRejection rejection;
    };
    auto const rows = std::array {
        Row { "1;1;1;@;@;@;0;2;@", PresentationStateRejection::MissingField },
        Row { "1;1;1;@;@;@;0;2", PresentationStateRejection::MissingField },
        Row { ";1;1;@;@;@;0;2;@;BBBB", PresentationStateRejection::MalformedNumber },
        Row { "1;x;1;@;@;@;0;2;@;BBBB", PresentationStateRejection::MalformedNumber },
        Row { "99999;1;1;@;@;@;0;2;@;BBBB", PresentationStateRejection::MalformedNumber },
        Row { "1;1;1;@;@;@;4;2;@;BBBB", PresentationStateRejection::MalformedNumber },
        Row { "1;1;1;`;@;@;0;2;@;BBBB", PresentationStateRejection::MalformedFlags },
        Row { "1;1;1;@;B;@;0;2;@;BBBB", PresentationStateRejection::MalformedFlags },
        Row { "1;1;1;@;@;P;0;2;@;BBBB", PresentationStateRejection::MalformedFlags },
        Row { "1;1;1;@;@;@@;0;2;@;BBBB", PresentationStateRejection::MalformedFlags },
        Row { "1;1;1;@;@;@;0;2;@;BBB", PresentationStateRejection::MalformedCharset },
        Row { "1;1;1;@;@;@;0;2;@;BBBBB", PresentationStateRejection::MalformedCharset },
        Row { "1;1;1;@;@;@;0;2;@;BBB%", PresentationStateRejection::MalformedCharset },
    };
    for (auto const& row: rows)
    {
        INFO(row.payload);
        auto const info = decodeCursorInformation(row.payload);
        REQUIRE_FALSE(info.has_value());
        CHECK(info.error() == row.rejection);
    }
}

TEST_CASE("PresentationState.decodeCursorInformation accepts the VT520's invisible bit",
          "[presentation-state]")
{
    auto const info = decodeCursorInformation("1;1;1;P;@;@;0;2;@;BBBB");
    REQUIRE(info.has_value());
    CHECK(info->rendition == 0x10);
}

TEST_CASE("PresentationState.decodeTabStops", "[presentation-state]")
{
    CHECK(decodeTabStops("") == std::vector<ColumnOffset> {});
    CHECK(decodeTabStops("9") == std::vector { ColumnOffset(8) });
    CHECK(decodeTabStops("17/1/9/9") == std::vector { ColumnOffset(0), ColumnOffset(8), ColumnOffset(16) });

    for (auto const payload: { "0", "1//9", "1/9/", "/1", "1;9", "99999" })
    {
        INFO(payload);
        CHECK(decodeTabStops(payload) == std::unexpected { PresentationStateRejection::MalformedNumber });
    }
}

TEST_CASE("PresentationState.charsetOfDesignator", "[presentation-state]")
{
    CHECK(charsetOfDesignator("B", false) == CharsetId::USASCII);
    CHECK(charsetOfDesignator("0", false) == CharsetId::Special);
    // `A` is British as a 94-set and Latin-1 as a 96-set: Scss decides which.
    CHECK(charsetOfDesignator("A", false) == CharsetId::British);
    CHECK(charsetOfDesignator("A", true) == CharsetId::ISOLatin1Supplemental);
    CHECK(charsetOfDesignator("B", true) == std::nullopt);
    CHECK(charsetOfDesignator("%5", false) == std::nullopt);
    CHECK(charsetOfDesignator("<", false) == std::nullopt);
}
