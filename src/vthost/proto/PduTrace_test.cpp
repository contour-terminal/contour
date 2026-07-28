// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/proto/Pdu.h>
#include <vthost/proto/PduTrace.h>

using namespace vthost::proto;
using namespace std::string_view_literals;

namespace
{
/// A SessionState carrying content the trace must not leak. Built field-by-field rather than
/// with designated initializers: the interesting fields straddle several the test does not care
/// about, and the pedantic build requires designators to be contiguous.
[[nodiscard]] SessionState sampleSessionState()
{
    auto value = SessionState {};
    value.session = 4;
    value.columns = 80;
    value.lines = 24;
    value.title = "secret-project";
    value.cwd = "file:///home/user/secret";
    return value;
}

/// A one-line Delta with a distinctive cursor, built for the same reason as above.
[[nodiscard]] Delta sampleDelta()
{
    auto value = Delta {};
    value.session = 7;
    value.generation = 3;
    value.seqno = 98;
    value.lines = std::vector<WireLine>(1);
    return value;
}

/// One example of every catalog alternative, so the tests below can sweep the whole variant
/// instead of naming alternatives one at a time (and silently missing the 18th).
[[nodiscard]] std::vector<std::pair<PduType, DecodedPdu>> everyAlternative()
{
    return {
        { PduType::Invalid, DecodedPdu { Invalid { .ident = 250 } } },
        { PduType::ClientHello,
          DecodedPdu { ClientHello { .codecVersion = 12,
                                     .token = "s3cr3t",
                                     // The settings block is the user's configuration; the sweep below proves
                                     // the trace summarizes rather than transcribes it.
                                     .sessionSettings =
                                         WireSessionSettings { .historyLineCount = 5000,
                                                               .wordDelimiters = "secret-delimiters" } } } },
        { PduType::ServerHello, DecodedPdu { ServerHello { .codecVersion = 12 } } },
        { PduType::Input,
          DecodedPdu {
              Input { .session = 4, .data = { std::byte { 'h' }, std::byte { 'i' }, std::byte { '!' } } } } },
        { PduType::ResizeRequest, DecodedPdu { ResizeRequest { .columns = 120, .lines = 40 } } },
        { PduType::ResizePane, DecodedPdu { ResizePane { .session = 4, .columns = 60, .lines = 20 } } },
        { PduType::FetchImage, DecodedPdu { FetchImage { .session = 4, .imageId = 9 } } },
        { PduType::ImageData,
          DecodedPdu { ImageData {
              .imageId = 9, .format = 1, .width = 32, .height = 16, .data = std::vector<std::byte>(64) } } },
        { PduType::ImageGone, DecodedPdu { ImageGone { .imageId = 9 } } },
        { PduType::SessionState, DecodedPdu { sampleSessionState() } },
        { PduType::Delta, DecodedPdu { sampleDelta() } },
        { PduType::SessionBell, DecodedPdu { SessionBell { .session = 4 } } },
        { PduType::SessionNotify,
          DecodedPdu { SessionNotify { .session = 4, .title = "secret-title", .body = "secret-body" } } },
        { PduType::SessionClipboard,
          DecodedPdu { SessionClipboard { .session = 4, .selection = "c", .data = "/home/user/secret" } } },
        { PduType::LayoutState,
          DecodedPdu { LayoutState { .window = 1, .activeTab = 0, .tabs = std::vector<WireTab>(2) } } },
        { PduType::CreateTab, DecodedPdu { CreateTab { .session = 4 } } },
        { PduType::SplitPane, DecodedPdu { SplitPane { .session = 4, .orientation = 1, .ratio = 5000 } } },
        { PduType::ClosePane, DecodedPdu { ClosePane { .session = 4 } } },
        { PduType::NewWindow, DecodedPdu { NewWindow {} } },
        { PduType::ResizeSplit,
          DecodedPdu { ResizeSplit { .firstSession = 4, .secondSession = 7, .ratio = 6000 } } },
    };
}
} // namespace

TEST_CASE("typeOf maps every alternative onto its catalog tag", "[vthost][proto]")
{
    // The sweep is only meaningful if it really covers the variant; TraceTable's static_assert
    // guards the table, this guards the fixture.
    auto const examples = everyAlternative();
    REQUIRE(examples.size() == std::variant_size_v<DecodedPdu>);

    for (auto const& [expected, pdu]: examples)
        CHECK(typeOf(pdu) == expected);
}

TEST_CASE("every PDU type has a distinct trace name", "[vthost][proto]")
{
    auto names = std::vector<std::string_view> {};
    for (auto const& [tag, pdu]: everyAlternative())
    {
        auto const name = toString(tag);
        CHECK(name != "Unknown"sv);
        names.push_back(name);
    }

    std::ranges::sort(names);
    CHECK(std::ranges::adjacent_find(names) == names.end());
}

TEST_CASE("an off-catalog PDU type renders as Unknown", "[vthost][proto]")
{
    CHECK(toString(static_cast<PduType>(200)) == "Unknown"sv);
}

TEST_CASE("summarize renders each PDU's identifying fields", "[vthost][proto]")
{
    auto const summaryOf = [](PduType wanted) {
        for (auto const& [tag, pdu]: everyAlternative())
            if (tag == wanted)
                return summarize(pdu);
        return std::string { "<missing>" };
    };

    SECTION("Delta reports its cursor and its batch sizes")
    {
        auto const text = summaryOf(PduType::Delta);
        CHECK(text.contains("session=7"));
        CHECK(text.contains("gen=3"));
        CHECK(text.contains("seq=98"));
        CHECK(text.contains("lines=1"));
    }

    SECTION("Input reports a byte count")
    {
        auto const text = summaryOf(PduType::Input);
        CHECK(text.contains("session=4"));
        CHECK(text.contains("bytes=3"));
    }

    SECTION("LayoutState reports its window and tab count")
    {
        auto const text = summaryOf(PduType::LayoutState);
        CHECK(text.contains("window=1"));
        CHECK(text.contains("tabs=2"));
    }

    SECTION("ImageData reports the pixel geometry and payload size")
    {
        auto const text = summaryOf(PduType::ImageData);
        CHECK(text.contains("image=9"));
        CHECK(text.contains("32x16px"));
        CHECK(text.contains("bytes=64"));
    }

    SECTION("each session event names its session and sizes its payload")
    {
        // The kind is the PDU's own name now, so there is no kind field left to render -- and the
        // payloads stay sizes, since a notification body and a clipboard write are user content.
        CHECK(summaryOf(PduType::SessionBell).contains("session=4"));
        CHECK(summaryOf(PduType::SessionNotify).contains("title=12ch"));
        CHECK(summaryOf(PduType::SessionNotify).contains("body=11ch"));
        CHECK(summaryOf(PduType::SessionClipboard).contains("selection=c"));
        CHECK(summaryOf(PduType::SessionClipboard).contains("data=17ch"));
    }

    SECTION("Invalid carries the peer's raw ident")
    {
        CHECK(summaryOf(PduType::Invalid).contains("ident=250"));
    }

    SECTION("payloadless PDUs summarize to nothing")
    {
        CHECK(summaryOf(PduType::NewWindow).empty());
    }

    SECTION("CreateTab names the window it targets")
    {
        // Not payloadless any more: it carries the session whose window the tab belongs in, and a
        // trace that omitted it could not tell two windows' tab requests apart.
        CHECK(summaryOf(PduType::CreateTab) == "session=4");
    }
}

TEST_CASE("the PDU trace never reveals payload contents", "[vthost][proto]")
{
    // A trace of a busy session must not become a transcript. This is the regression test for
    // that rule; it is cheap, and it is the one property a careless new TraceTable row breaks.
    for (auto const& [tag, pdu]: everyAlternative())
    {
        auto const text = summarize(pdu);
        INFO("PDU: " << toString(tag));
        CHECK_FALSE(text.contains("s3cr3t"));            // the ClientHello preshared token
        CHECK_FALSE(text.contains("secret-delimiters")); // the ClientHello session settings
        CHECK_FALSE(text.contains("hi!"));               // Input keystrokes
        CHECK_FALSE(text.contains("secret-project"));    // the window title
        CHECK_FALSE(text.contains("/home/user/secret")); // the OSC 7 working directory
        // NOT the bare word "body": SessionNotify's own summary labels a field that way, so a
        // sentinel has to be one no field NAME can match.
        CHECK_FALSE(text.contains("secret-title")); // a desktop notification's title
        CHECK_FALSE(text.contains("secret-body"));  // a desktop notification's body
    }

    // The token's PRESENCE is still reported — that is what makes an auth failure diagnosable, and
    // the same holds for the settings block: "did this client state a preference?" is the question a
    // session that emulated unexpectedly needs answered.
    CHECK(summarize(DecodedPdu { ClientHello { .token = "s3cr3t" } }).contains("token=yes"));
    CHECK(summarize(DecodedPdu { ClientHello {} }).contains("token=no"));
    CHECK(summarize(DecodedPdu { ClientHello { .sessionSettings = WireSessionSettings {} } })
              .contains("settings=yes"));
    CHECK(summarize(DecodedPdu { ClientHello {} }).contains("settings=no"));
}

TEST_CASE("traceLine pins the one-line trace format", "[vthost][proto]")
{
    CHECK(traceLine(Direction::Recv, 12, DecodedPdu { ResizeRequest { .columns = 120, .lines = 40 } }, 11)
          == "recv #12 ResizeRequest cols=120 lines=40 (11 bytes)");

    // A payloadless PDU must not leave a dangling separator before the byte count.
    CHECK(traceLine(Direction::Send, 0, DecodedPdu { NewWindow {} }, 4) == "send #0 NewWindow (4 bytes)");
}

TEST_CASE("the catalog's tags are contiguous from zero", "[vthost][proto]")
{
    // While the protocol is unreleased a retired PDU leaves no gap -- a gap only earns its keep
    // against a deployed peer that still remembers the old meaning, and there is none. Retiring
    // SessionEvent therefore freed tag 10 for SessionBell rather than stranding it.
    //
    // Asserted rather than merely documented, because a hole is invisible: nothing else in the codec
    // notices one, and the next reader would sensibly assume tags are already stable. When the
    // protocol ships, THIS is the test to delete -- and its deletion is the moment tags become
    // stable, which is a more honest marker than a comment.
    auto tags = std::vector<std::size_t> {};
    for (auto const& [tag, pdu]: everyAlternative())
        tags.push_back(static_cast<std::size_t>(std::to_underlying(tag)));
    std::ranges::sort(tags);

    REQUIRE(tags.size() == std::variant_size_v<DecodedPdu>);
    for (auto const expected: std::views::iota(std::size_t { 0 }, tags.size()))
        CHECK(tags[expected] == expected);
}
