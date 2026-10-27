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

#include <vthost/proto/Pdu.hpp>
#include <vthost/proto/PduTrace.hpp>
#include <vthost/testing/PduExamples.hpp>

using namespace vthost::proto;
using namespace std::string_view_literals;

using vthost::testing::everyAlternative;

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

TEST_CASE("nameOfIdent names a catalog tag, and numbers any other ident", "[vthost][proto]")
{
    for (auto const& [tag, pdu]: everyAlternative())
        if (tag != PduType::Invalid)
            CHECK(nameOfIdent(std::to_underlying(tag)) == toString(tag));
    CHECK(nameOfIdent(0) == "0");
    CHECK(nameOfIdent(200) == "200");
    CHECK(nameOfIdent(uint64_t { 1 } << 40) == "1099511627776");
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
        CHECK(text.contains("blocks=1"));
        CHECK(text.contains("retired=1"));
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

    SECTION("PerformOperation names its session and operation and sizes its text")
    {
        CHECK(summaryOf(PduType::PerformOperation) == "session=4 operation=3 text=18B row=0@0");
    }

    SECTION("CreateTab names the window it targets")
    {
        // Not payloadless any more: it carries the session whose window the tab belongs in, and a
        // trace that omitted it could not tell two windows' tab requests apart.
        CHECK(summaryOf(PduType::CreateTab) == "session=4 profile=\"work\"");
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
        CHECK_FALSE(text.contains("secret-title"));       // a desktop notification's title
        CHECK_FALSE(text.contains("secret-body"));        // a desktop notification's body
        CHECK_FALSE(text.contains("secret-command"));     // a command line a shell reported
        CHECK_FALSE(text.contains("secret-dir"));         // the directory it ran in
        CHECK_FALSE(text.contains("secret-screen-text")); // what a client asked the host to write
        // The daemon's machine id and host name: they say which machine a session runs on.
        CHECK_FALSE(text.contains("secret-machine-id"));
        CHECK_FALSE(text.contains("secret-host"));
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

TEST_CASE("the hellos trace their ranges and capabilities", "[vthost][proto][negotiation]")
{
    CHECK(summarize(DecodedPdu {
              ClientHello { .versions = { 4, 6 }, .capabilities = { .idents = { 1, 4711 } }, .token = "t" } })
          == "versions=4-6 caps=1,4711 token=yes settings=no role=attach area=none "
             "intent=all-windows windows=[] profile=\"\"");
    CHECK(summarize(DecodedPdu { ClientHello {} })
          == "versions=4-4 caps=none token=no settings=no role=attach area=none intent=all-windows "
             "windows=[] profile=\"\"");
    CHECK(summarize(DecodedPdu { ClientHello { .attachIntent = { .kind = AttachIntentKind::Launch },
                                               .profileName = "main" } })
              .ends_with("intent=launch windows=[] profile=\"main\""));
    CHECK(summarize(DecodedPdu { ClientHello {
                        .attachIntent = { .kind = AttachIntentKind::Resume, .windows = { 1, 7 } } } })
              .contains("intent=resume windows=[1,7]"));
    CHECK(summarize(DecodedPdu { ClientHello { .role = ConnectionRole::Control } }).contains("role=control"));
    CHECK(summarize(DecodedPdu { ServerHello {
              .agreedVersion = 4, .capabilities = { .idents = { 1 } }, .machineId = "m", .hostname = "h" } })
          == "layout=negotiating agreed=4 versions=4-4 caps=1 release=unknown followed=[] "
             "instance=0000000000000000 restored=0/0");
    CHECK(summarize(DecodedPdu { ServerHello { .agreedVersion = 4, .followedWindows = { 1, 3 } } })
              .contains("followed=[1,3]"));
    CHECK(summarize(DecodedPdu { ServerHello { .agreedVersion = 4, .daemonInstance = 0xC0FFEE } })
              .contains("instance=0000000000c0ffee"));
    CHECK(
        summarize(DecodedPdu { ServerHello { .agreedVersion = 4, .restoredWindows = 2, .restoredTabs = 7 } })
            .ends_with("restored=2/7"));
    CHECK(summarize(DecodedPdu { ServerHello { .agreedVersion = 4, .releaseVersion = "0.8.1" } })
              .contains("release=0.8.1"));
    CHECK(summarize(DecodedPdu { ServerHello { .layout = HelloLayout::VersionAlone, .codecVersion = 3 } })
          == "layout=version-alone version=3");
    CHECK(summarize(DecodedPdu { Unsupported { .refusedSerial = 9, .ident = 200 } })
          == "refused=9 ident=200");
}

TEST_CASE("the daemon-control PDUs are named, and a status reply never shows a path", "[vthost][proto]")
{
    CHECK(toString(PduType::StopDaemon) == "StopDaemon");
    CHECK(toString(PduType::DaemonStatusRequest) == "DaemonStatusRequest");
    CHECK(summarize(DecodedPdu { StopDaemon { .reason = 1 } }) == "reason=1");
    auto const text = summarize(DecodedPdu { DaemonStatusReply { .releaseVersion = "0.8.1",
                                                                 .processId = 4242,
                                                                 .socketPath = "/run/user/1000/contour/x",
                                                                 .executablePath = "/home/u/bin/contour",
                                                                 .logFile = "/home/u/x.log",
                                                                 .sessions = 3,
                                                                 .clients = 1 } });
    CHECK(text.contains("sessions=3 clients=1"));
    CHECK_FALSE(text.contains("/run/user"));
    CHECK_FALSE(text.contains("/home/u"));
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

TEST_CASE("the trace names the new PDUs without revealing a title", "[vthost][proto]")
{
    CHECK(summarize(DecodedPdu { EditTab { .session = 9, .kind = TabEditKind::SetColor, .color = 0x336699 } })
          == "session=9 set-color #336699");
    CHECK(summarize(DecodedPdu { EditTab { .session = 9, .kind = TabEditKind::SetTitle, .title = "secret" } })
          == "session=9 set-title title=6B");
    CHECK(summarize(DecodedPdu { EditTab { .session = 9, .kind = static_cast<TabEditKind>(0xEE) } })
          == "session=9 kind=238");
    CHECK(summarize(DecodedPdu { Heartbeat { .nonce = 42 } }) == "nonce=42");
    CHECK(summarize(DecodedPdu { MergeTabIntoSplit { .tabSession = 4, .targetSession = 9, .side = 2 } })
          == "tab=4 target=9 side=2");
    CHECK(summarize(DecodedPdu { CreateTab { .session = 3, .profileName = "ssh" } })
          == "session=3 profile=\"ssh\"");
    CHECK(summarize(DecodedPdu { NewWindow { .profileName = "y" } }) == "profile=\"y\"");
}

TEST_CASE("a ControlRequest's trace names the method but not the params", "[vthost][proto][control]")
{
    auto const line =
        traceLine(Direction::Recv,
                  4,
                  DecodedPdu { ControlRequest {
                      .requestId = 7, .method = "pane.send", .params = R"({"text":"secret"})" } },
                  40);
    CHECK(line.contains("pane.send"));
    CHECK(line.contains("id=7"));
    CHECK_FALSE(line.contains("secret"));
}

TEST_CASE("a control reply and event are traced by size only", "[vthost][proto][control]")
{
    CHECK(summarize(DecodedPdu { ControlReply { .requestId = 7, .body = "pane text" } })
          == "id=7 ok body=9B");
    CHECK(summarize(
              DecodedPdu { ControlReply { .requestId = 7, .outcome = ControlOutcome::Error, .body = "x" } })
          == "id=7 error body=1B");
    CHECK(summarize(DecodedPdu { ControlEvent { .subscription = 3, .event = "secret pane text" } })
          == "subscription=3 event=16B");
}

TEST_CASE("the ProgramStatus trace counts records and never shows their text",
          "[vthost][proto][programstatus]")
{
    auto const summary = summarize(DecodedPdu { ProgramStatus {
        .session = 2,
        .records = { WireProgramStatusRecord { .id = "x", .title = "secret", .message = "secret" } } } });
    CHECK(summary == "session=2 records=1");
}
