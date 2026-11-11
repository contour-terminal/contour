// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/UntrustedText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <ranges>
#include <vector>

using vtbackend::CodePointTreatment;
using vtbackend::ShownAs;
using vtbackend::treatmentOf;
using vtbackend::UntrustedTextFilter;
using vtbackend::UntrustedTextTreatments;

TEST_CASE("UntrustedText.rowsAreWellFormed", "[vtbackend][untrustedtext]")
{
    for (auto const& row: UntrustedTextTreatments)
    {
        CHECK(row.first <= row.last);
        CHECK(row.treatment != CodePointTreatment::Keep); // a code point in no row is kept already
    }
}

TEST_CASE("UntrustedText.treatmentOf", "[vtbackend][untrustedtext]")
{
    // Line breaks win over the control range that holds them.
    STATIC_CHECK(treatmentOf(U'\n') == CodePointTreatment::Space);
    STATIC_CHECK(treatmentOf(U'\r') == CodePointTreatment::Space);
    STATIC_CHECK(treatmentOf(0x2029) == CodePointTreatment::Space);
    STATIC_CHECK(treatmentOf(0x1B) == CodePointTreatment::Drop);
    STATIC_CHECK(treatmentOf(0x9B) == CodePointTreatment::Drop);
    STATIC_CHECK(treatmentOf(U'\t') == CodePointTreatment::Drop);

    // Every range the final review named as missing from one table or the other.
    for (auto const codePoint:
         { 0x061C,  0x2028,  0xE0001, 0x0600, 0x0605, 0x06DD, 0x070F, 0x08E2, 0x13430, 0x1343F, 0x1BCA0,
           0x1BCA3, 0x1D173, 0x1D17A, 0x115F, 0x1160, 0x3164, 0xFFA0, 0x202E, 0x2066,  0xFEFF })
        CHECK(treatmentOf(static_cast<char32_t>(codePoint)) != CodePointTreatment::Keep);

    // Variation selectors and ordinary text stay.
    for (auto const codePoint: { 0xFE0F, 0xFE00, 0xE0100, 0x180B, 0x0061, 0x00E9, 0x2714, 0x1F600 })
        CHECK(treatmentOf(static_cast<char32_t>(codePoint)) == CodePointTreatment::Keep);
}

TEST_CASE("UntrustedText.filterFoldsLineBreaksBetweenShownCodePoints", "[vtbackend][untrustedtext]")
{
    using enum CodePointTreatment;
    auto const shownOf = [](std::initializer_list<CodePointTreatment> treatments) {
        auto filter = UntrustedTextFilter {};
        auto shown = std::vector<ShownAs> {};
        for (auto const treatment: treatments)
            shown.push_back(filter.next(treatment));
        return shown;
    };
    // Leading and trailing breaks vanish; a run between two shown code points is one space, a drop inside it
    // changing nothing.
    CHECK(shownOf({ Space, Keep, Space, Drop, Space, Keep, Space })
          == std::vector { ShownAs::Nothing,
                           ShownAs::Itself,
                           ShownAs::Nothing,
                           ShownAs::Nothing,
                           ShownAs::Nothing,
                           ShownAs::AfterSpace,
                           ShownAs::Nothing });
    CHECK(shownOf({ Drop, Keep, Keep })
          == std::vector { ShownAs::Nothing, ShownAs::Itself, ShownAs::Itself });
}
