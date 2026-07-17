// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <vtconformance/Diagnostics.h>

using namespace std::string_view_literals;
using namespace vtconformance;

TEST_CASE("Diagnostics.classifyDiagnostic", "[vtconformance]")
{
    SECTION("each of the engine's four failure buckets is recognised")
    {
        auto const unknown = classifyDiagnostic("Unknown VT sequence: ESC Z"sv);
        REQUIRE(unknown);
        CHECK(unknown->kind == DiagnosticKind::Unknown);
        CHECK(unknown->sequence == "ESC Z");

        auto const unsupported = classifyDiagnostic("Unsupported VT sequence: CSI 22 ; 1 t"sv);
        REQUIRE(unsupported);
        CHECK(unsupported->kind == DiagnosticKind::Unsupported);

        auto const invalid = classifyDiagnostic("Invalid VT sequence: CSI 99 \" p"sv);
        REQUIRE(invalid);
        CHECK(invalid->kind == DiagnosticKind::Invalid);

        auto const parserError = classifyDiagnostic("Parser error: unexpected byte"sv);
        REQUIRE(parserError);
        CHECK(parserError->kind == DiagnosticKind::ParserError);
    }

    SECTION("an ordinary log line is not a diagnostic")
    {
        CHECK_FALSE(classifyDiagnostic("Terminal resized to 80x24"sv));
    }

    SECTION("trailing newlines are stripped from the sequence")
    {
        auto const diagnostic = classifyDiagnostic("Unknown VT sequence: ESC Z\n"sv);
        REQUIRE(diagnostic);
        CHECK(diagnostic->sequence == "ESC Z");
    }

    SECTION("a self-echoed DECRQCRA reply has its volatile request id folded away")
    {
        auto const diagnostic = classifyDiagnostic("Unknown VT sequence: DCS 3709 ! ~"sv);
        REQUIRE(diagnostic);
        CHECK(diagnostic->sequence == "DCS <id> ! ~");
    }
}

TEST_CASE("Diagnostics.canonicalizeSequence", "[vtconformance]")
{
    SECTION("the DECRQCRA-reply shape folds its request id to a placeholder")
    {
        CHECK(canonicalizeSequence("DCS 3709 ! ~"sv) == "DCS <id> ! ~");
        CHECK(canonicalizeSequence("DCS 1 ! ~"sv) == "DCS <id> ! ~");
        // Distinct ids collapse onto the same stable record, so the gap ratchet cannot flap.
        CHECK(canonicalizeSequence("DCS 3709 ! ~"sv) == canonicalizeSequence("DCS 42 ! ~"sv));
    }

    SECTION("sequences whose number is meaningful are left untouched")
    {
        CHECK(canonicalizeSequence("CSI ? 41 l"sv) == "CSI ? 41 l");
        CHECK(canonicalizeSequence("ESC Z"sv) == "ESC Z");
        CHECK(canonicalizeSequence("DCS z"sv) == "DCS z");     // no numeric id, not the reply shape
        CHECK(canonicalizeSequence("DCS ! ~"sv) == "DCS ! ~"); // empty id: not rewritten
    }
}

TEST_CASE("Diagnostics.DiagnosticsCollector captures what the engine ignores", "[vtconformance]")
{
    // This is the load-bearing contract of oracle A: if the engine's log prefixes ever drift away
    // from what `classifyDiagnostic` expects, the conformance report would quietly turn into an
    // all-clear. Driving a real MockTerm here — rather than feeding the classifier a hand-written
    // string — is what makes that drift impossible to miss.
    auto collector = DiagnosticsCollector {};
    auto mock =
        vtbackend::MockTerm { vtbackend::PageSize { vtbackend::LineCount(4), vtbackend::ColumnCount(20) } };

    // `CSI _` is a control sequence no standard defines: ECMA-48 assigns CSI final bytes up to 0x5A
    // and reserves 0x70..0x7E for private use, leaving 0x5B..0x5F (`[ \ ] ^ _`) unassigned. So the
    // engine cannot recognise it, and -- unlike a sequence that merely is not implemented *yet* --
    // it will not start recognising it either.
    //
    // That distinction is the point. This test used to drive DECID (`ESC Z`) and assert the engine
    // ignored it; implementing DECID then broke the test, which had quietly pinned a *gap* rather
    // than the contract it meant to. A test that fails when the engine improves is testing the wrong
    // thing.
    constexpr auto UnassignedSequence = "\033[_"sv;

    SECTION("a sequence the engine does not implement shows up")
    {
        mock.writeToScreen(UnassignedSequence);

        auto const diagnostics = collector.collected();
        CHECK_FALSE(diagnostics.empty());
    }

    SECTION("plain text produces no diagnostics")
    {
        mock.writeToScreen("hello"sv);
        CHECK(collector.collected().empty());
    }

    SECTION("a repeated gap is counted, not repeated")
    {
        mock.writeToScreen("\033[_\033[_\033[_"sv);

        auto const diagnostics = collector.collected();
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].count == 3);
    }

    SECTION("clear() forgets everything")
    {
        mock.writeToScreen(UnassignedSequence);
        REQUIRE_FALSE(collector.collected().empty());

        collector.clear();
        CHECK(collector.collected().empty());
    }
}
