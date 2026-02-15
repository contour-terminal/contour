// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/DesktopNotification.h>

#include <crispy/base64.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("DesktopNotification.ParseSimpleTitle", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=test1;Hello World");
    CHECK(n.identifier == "test1");
    CHECK(n.title == "Hello World");
    CHECK(n.body.empty());
    CHECK(n.urgency == NotificationUrgency::Normal);
    CHECK(n.occasion == DisplayOccasion::Always);
    CHECK(n.done == true);
    CHECK(n.currentPayload == NotificationPayloadType::Title);
}

TEST_CASE("DesktopNotification.ParseMultipleMetadataKeys", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=test2:u=2:o=unfocused:f=myapp:w=5000;Critical Alert");
    CHECK(n.identifier == "test2");
    CHECK(n.title == "Critical Alert");
    CHECK(n.urgency == NotificationUrgency::Critical);
    CHECK(n.occasion == DisplayOccasion::Unfocused);
    CHECK(n.applicationName == "myapp");
    CHECK(n.timeout == 5000);
}

TEST_CASE("DesktopNotification.ParseBodyPayload", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=test3:p=body;This is the body");
    CHECK(n.identifier == "test3");
    CHECK(n.title.empty());
    CHECK(n.body == "This is the body");
    CHECK(n.currentPayload == NotificationPayloadType::Body);
}

TEST_CASE("DesktopNotification.ParseClosePayload", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=test4:p=close;");
    CHECK(n.identifier == "test4");
    CHECK(n.currentPayload == NotificationPayloadType::Close);
}

TEST_CASE("DesktopNotification.ParseQueryPayload", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=q1:p=?;");
    CHECK(n.identifier == "q1");
    CHECK(n.currentPayload == NotificationPayloadType::Query);
}

TEST_CASE("DesktopNotification.ParseAlivePayload", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=a1:p=alive;");
    CHECK(n.identifier == "a1");
    CHECK(n.currentPayload == NotificationPayloadType::Alive);
}

TEST_CASE("DesktopNotification.ParseBase64Encoded", "[DesktopNotification]")
{
    // "Hello" in base64 is "SGVsbG8="
    auto const encoded = crispy::base64::encode("Hello");
    auto const raw = "i=b64test:e=1;" + encoded;
    auto const n = parseOSC99(raw);
    CHECK(n.identifier == "b64test");
    CHECK(n.base64Encoded == true);
    CHECK(n.title == "Hello");
}

TEST_CASE("DesktopNotification.ParseChunkingNotDone", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=chunk1:d=0;Hello ");
    CHECK(n.identifier == "chunk1");
    CHECK(n.done == false);
    CHECK(n.title == "Hello ");
}

TEST_CASE("DesktopNotification.ParseChunkingDone", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=chunk1:d=1;World");
    CHECK(n.identifier == "chunk1");
    CHECK(n.done == true);
    CHECK(n.title == "World");
}

TEST_CASE("DesktopNotification.ParseUrgencyLevels", "[DesktopNotification]")
{
    SECTION("Low")
    {
        auto const n = parseOSC99("i=u0:u=0;low");
        CHECK(n.urgency == NotificationUrgency::Low);
    }
    SECTION("Normal")
    {
        auto const n = parseOSC99("i=u1:u=1;normal");
        CHECK(n.urgency == NotificationUrgency::Normal);
    }
    SECTION("Critical")
    {
        auto const n = parseOSC99("i=u2:u=2;crit");
        CHECK(n.urgency == NotificationUrgency::Critical);
    }
}

TEST_CASE("DesktopNotification.ParseOccasionTypes", "[DesktopNotification]")
{
    SECTION("Always")
    {
        auto const n = parseOSC99("i=o1:o=always;test");
        CHECK(n.occasion == DisplayOccasion::Always);
    }
    SECTION("Unfocused")
    {
        auto const n = parseOSC99("i=o2:o=unfocused;test");
        CHECK(n.occasion == DisplayOccasion::Unfocused);
    }
    SECTION("Invisible")
    {
        auto const n = parseOSC99("i=o3:o=invisible;test");
        CHECK(n.occasion == DisplayOccasion::Invisible);
    }
}

TEST_CASE("DesktopNotification.ParseActivationFlags", "[DesktopNotification]")
{
    SECTION("Focus only")
    {
        auto const n = parseOSC99("i=af1:a=focus;test");
        CHECK(n.focusOnActivation == true);
        CHECK(n.reportOnActivation == false);
    }
    SECTION("Report only")
    {
        auto const n = parseOSC99("i=af2:a=report;test");
        CHECK(n.focusOnActivation == false);
        CHECK(n.reportOnActivation == true);
    }
    SECTION("Both focus and report")
    {
        auto const n = parseOSC99("i=af3:a=focus,report;test");
        CHECK(n.focusOnActivation == true);
        CHECK(n.reportOnActivation == true);
    }
}

TEST_CASE("DesktopNotification.ParseCloseEventRequested", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=c1:c=1;test");
    CHECK(n.closeEventRequested == true);
}

TEST_CASE("DesktopNotification.ParseEmptyPayload", "[DesktopNotification]")
{
    auto const n = parseOSC99("i=empty;");
    CHECK(n.identifier == "empty");
    CHECK(n.title.empty());
    CHECK(n.body.empty());
}

TEST_CASE("DesktopNotification.ParseNoSemicolon", "[DesktopNotification]")
{
    // Only metadata, no semicolon separator — treated as metadata-only with empty payload.
    auto const n = parseOSC99("i=nosemi");
    CHECK(n.identifier == "nosemi");
    CHECK(n.title.empty());
}

TEST_CASE("DesktopNotification.UnknownKeysIgnored", "[DesktopNotification]")
{
    // Unknown keys like "z" should be silently ignored.
    auto const n = parseOSC99("i=unk:z=whatever:u=2;test");
    CHECK(n.identifier == "unk");
    CHECK(n.urgency == NotificationUrgency::Critical);
    CHECK(n.title == "test");
}

TEST_CASE("DesktopNotification.QueryResponse", "[DesktopNotification]")
{
    auto const response = buildOSC99QueryResponse("test-id");
    CHECK(response.find("99;i=test-id:p=?;") != std::string::npos);
    CHECK(response.find("a=focus,report") != std::string::npos);
    CHECK(response.find("u=0,1,2") != std::string::npos);
    CHECK(response.find("c=1") != std::string::npos);
    CHECK(response.find("w=1") != std::string::npos);
}
