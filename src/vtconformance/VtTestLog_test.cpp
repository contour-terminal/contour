// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vtconformance/VtTestLog.h>

using namespace std::string_view_literals;
using namespace vtconformance;

TEST_CASE("VtTestLog.decodeBytes", "[vtconformance]")
{
    SECTION("decimal escapes become raw bytes")
    {
        CHECK(decodeBytes("<27> [ 0 c"sv) == "\033[0c");
    }

    SECTION("a literal space is encoded as <32>, so spacing is unambiguous")
    {
        // vttest separates byte tokens with spaces, which would make a real space byte invisible.
        // It therefore encodes it as <32>. Getting this wrong would silently corrupt every golden.
        CHECK(decodeBytes("A <32> B"sv) == "A B");
    }

    SECTION("DECRQSS is round-tripped intact")
    {
        CHECK(decodeBytes(R"(<27> P $ q " p <27> \)"sv) == "\033P$q\"p\033\\");
    }

    SECTION("an empty field decodes to nothing")
    {
        CHECK(decodeBytes(""sv).empty());
    }
}

TEST_CASE("VtTestLog.parseVtTestLog", "[vtconformance]")
{
    constexpr auto Transcript = "Note: Setup Terminal with test-defaults\n"
                                "Send: <27> [ 0 c \n"
                                "Wait: 1\n"
                                "Read: <27> [ ? 6 5 ; 1 c \n"
                                "Done: 1\n"
                                "Text: VT100 test program\n"sv;

    auto const records = parseVtTestLog(Transcript);
    REQUIRE(records.size() == 6);

    CHECK(records[0].kind == VtTestRecordKind::Note);
    CHECK(records[0].payload == "Setup Terminal with test-defaults");

    CHECK(records[1].kind == VtTestRecordKind::Send);
    CHECK(records[1].payload == "\033[0c");

    CHECK(records[3].kind == VtTestRecordKind::Read);
    CHECK(records[3].payload == "\033[?65;1c");

    CHECK(records[5].kind == VtTestRecordKind::Text);
    CHECK(records[5].payload == "VT100 test program");
}

TEST_CASE("VtTestLog.extractQueries", "[vtconformance]")
{
    SECTION("an answered query pairs the request with the reply")
    {
        auto const records = parseVtTestLog("Send: <27> [ 0 c \n"
                                            "Wait: 1\n"
                                            "Read: <27> [ ? 6 5 ; 1 c \n"
                                            "Done: 1\n"sv);
        auto const queries = extractQueries(records);

        REQUIRE(queries.size() == 1);
        CHECK(queries[0].request == "\033[0c");
        CHECK(queries[0].reply == "\033[?65;1c");
        CHECK(queries[0].answered());
    }

    SECTION("an UNANSWERED query is the zero-oracle failure signal")
    {
        // This is the whole point of reading vttest's transcript: a Wait: with no bytes in its
        // Read: means the terminal never replied. That is a conformance bug, and it needs no
        // golden file to detect.
        auto const records = parseVtTestLog("Send: <27> P $ q \" p <27> \\ \n"
                                            "Wait: 2\n"
                                            "Read: \n"
                                            "Done: 2\n"sv);
        auto const queries = extractQueries(records);

        REQUIRE(queries.size() == 1);
        CHECK(queries[0].request == "\033P$q\"p\033\\");
        CHECK(queries[0].reply.empty());
        CHECK_FALSE(queries[0].answered());
    }

    SECTION("a bare 'Read:' with no trailing space is still an unanswered query")
    {
        auto const records = parseVtTestLog("Send: <27> [ c \nWait: 1\nRead:\nDone: 1\n"sv);
        auto const queries = extractQueries(records);

        REQUIRE(queries.size() == 1);
        CHECK_FALSE(queries[0].answered());
    }

    SECTION("menu keystrokes are not mistaken for query replies")
    {
        // A Read: outside a Wait:/Done: bracket is vttest reading OUR keystroke, not the
        // terminal's answer. Pairing it with a query would invent a reply that never existed.
        auto const records = parseVtTestLog("Read: 1 \nSend: <27> # 8 \n"sv);
        CHECK(extractQueries(records).empty());
    }

    SECTION("a wait for the HUMAN is not a query the terminal failed to answer")
    {
        // vttest brackets its waits for a key press exactly as it brackets a query reply, so keying
        // off `Wait:` alone would report every "Push the RETURN key" prompt in the suite as a
        // terminal that went silent. A query is a `Wait:` preceded by a `Send:` — a control
        // sequence. This distinction is what keeps the whole zero-oracle signal trustworthy.
        auto const records =
            parseVtTestLog("Send: <27> [ 2 0 h \n"
                           "Data: N e w L i n e <32> m o d e <32> s e t . <32> P u s h <32> R E T \n"
                           "Wait: 5\n"
                           "Read: \n"
                           "Done: 5\n"sv);

        CHECK(extractQueries(records).empty());
    }

    SECTION("an ENQ answerback wait is not a query either")
    {
        // vttest sends ENQ as `Data:`, not `Send:`, and an empty answerback is the correct default
        // for every mainstream terminal. Reporting it as a failure would be a false alarm.
        auto const records = parseVtTestLog("Data: <5> \nWait: 4\nRead: \nDone: 4\n"sv);
        CHECK(extractQueries(records).empty());
    }
}

TEST_CASE("VtTestLog.extractVerdicts", "[vtconformance]")
{
    SECTION("vttest's own pass and fail phrasings are recognised")
    {
        auto const records = parseVtTestLog("Text: Report is: <27>[?65;1c -- OK\n"
                                            "Text: Report is: <27>[?1;2c -- Not expected\n"
                                            "Text: Autowrap-pending: failed 3 of 5 tries\n"
                                            "Text: The screen should show a box.\n"sv);
        auto const verdicts = extractVerdicts(records);

        REQUIRE(verdicts.size() == 3);
        CHECK(verdicts[0].passed);
        CHECK_FALSE(verdicts[1].passed);
        CHECK_FALSE(verdicts[2].passed);
    }

    SECTION("a verdict from show_result() counts, and it is logged as a Note")
    {
        // vttest's show_result() writes `Note: result <verdict>` (main.c:2104-2109), not `Text:`. Most
        // of its verdicts arrive that way -- 20 of chapter 06's 22 -- so a reader that scans only
        // `Text:` reads a tenth of them and calls the chapter self-checked. Both LNM failures lived
        // here, unseen.
        auto const records = parseVtTestLog("Note: result  -- Not expected\n"
                                            "Note: result  -- OK\n"sv);
        auto const verdicts = extractVerdicts(records);

        REQUIRE(verdicts.size() == 2);
        CHECK_FALSE(verdicts[0].passed);
        CHECK(verdicts[1].passed);
    }

    SECTION("a Note that is not a result is not a verdict")
    {
        // `Note:` is also vttest's bookkeeping, and it names the chapter it is about to run. Scanning
        // every Note would let a chapter's own title decide its verdict -- "Test of known bugs" is
        // fine, but a menu entry containing "failed" would fail the chapter for being called that.
        auto const records = parseVtTestLog("Note: choice 9: Test of known bugs\n"
                                            "Note: Selecting all choices\n"
                                            "Note: choice 2: A test that failed to be renamed\n"sv);

        CHECK(extractVerdicts(records).empty());
    }

    SECTION("'No communication errors' is a pass, not a fail")
    {
        // Order in the pattern table is load-bearing: this line contains the failure phrasing
        // "ommunication errors" as a substring. A naive scan would report a passing terminal as
        // broken.
        auto const records = parseVtTestLog("Text: No communication errors\n"sv);
        auto const verdicts = extractVerdicts(records);

        REQUIRE(verdicts.size() == 1);
        CHECK(verdicts[0].passed);
    }

    SECTION("ordinary instruction text is not a verdict")
    {
        auto const records = parseVtTestLog("Text: Push <RETURN>\nText: Choose test type:\n"sv);
        CHECK(extractVerdicts(records).empty());
    }
}

TEST_CASE("VtTestLog.prettyBytes", "[vtconformance]")
{
    CHECK(prettyBytes("\033[0c"sv) == "ESC[0c");
    CHECK(prettyBytes("\033P$q\"p\033\\"sv) == R"(ESCP$q"pESC\)");
    CHECK(prettyBytes("\x01"sv) == "<0x01>");
}
