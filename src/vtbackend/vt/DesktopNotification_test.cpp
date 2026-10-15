// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/vt/DesktopNotification.hpp>

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;

namespace
{

/// A terminal that records the OSC 99 notifications it raises.
class NotificationTerm final: public MockTerm<>
{
  public:
    NotificationTerm(): MockTerm<> { PageSize { .lines = LineCount(5), .columns = ColumnCount(40) } } {}

    void showDesktopNotification(DesktopNotification const& notification) override
    {
        _shown.push_back(notification);
    }

    /// @return The notifications raised so far, oldest first.
    [[nodiscard]] std::vector<DesktopNotification> const& shown() const noexcept { return _shown; }

    /// @return The terminal's OSC 99 bookkeeping.
    [[nodiscard]] DesktopNotificationManager& manager() noexcept
    {
        return terminal.desktopNotificationManager();
    }

    /// Writes @p body as the application would: wrapped in `OSC 99 ; ... ST`.
    void osc99(std::string_view body) { writeToScreen(std::format("\033]99;{}\033\\", body)); }

    /// @return The identifiers a `p=alive` query is answered with.
    [[nodiscard]] std::vector<std::string> aliveIdentifiers()
    {
        discardPendingReplies();
        osc99("i=query:p=alive;");
        terminal.flushInput();
        auto const& reply = replyData();
        auto const prefix = std::string_view { "p=alive;" };
        auto const begin = reply.find(prefix);
        REQUIRE(begin != std::string::npos);
        auto const listBegin = begin + prefix.size();
        auto const list =
            std::string_view { reply }.substr(listBegin, reply.find('\033', listBegin) - listBegin);
        auto identifiers = std::vector<std::string> {};
        for (auto const part: std::views::split(list, ','))
            identifiers.emplace_back(std::string_view { part });
        return identifiers;
    }

  private:
    std::vector<DesktopNotification> _shown;
};

/// @return Whether @p identifiers holds @p identifier.
bool holds(std::vector<std::string> const& identifiers, std::string const& identifier)
{
    return std::ranges::contains(identifiers, identifier);
}

/// How far past a bound a test's burst of new identifiers goes.
constexpr auto Overshoot = size_t { 72 };

} // namespace

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
    auto const encoded = core::base64::encode("Hello");
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
    SECTION("Default (no a= key)")
    {
        auto const n = parseOSC99("i=af4;test");
        CHECK(n.focusOnActivation == true);
        CHECK(n.reportOnActivation == false);
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
    CHECK(response.contains("99;i=test-id:p=?;"));
    CHECK(response.contains("a=focus,report"));
    CHECK(response.contains("u=0,1,2"));
    CHECK(response.contains("c=1"));
    CHECK(response.contains("w=1"));
}

TEST_CASE("DesktopNotification.CloseResponseObserved", "[DesktopNotification]")
{
    // A close the terminal watched happen carries no extra payload, and must not start carrying one.
    CHECK(buildOSC99CloseResponse("test-id", CloseReport::Observed) == "99;i=test-id:p=close;");
}

TEST_CASE("DesktopNotification.CloseResponseUntracked", "[DesktopNotification]")
{
    // A close nobody reported is marked as such. The protocol defines `untracked` for a platform
    // whose desktop cannot report closure -- which is every sandboxed Contour, because
    // org.freedesktop.portal.Notification has no close signal at all.
    CHECK(buildOSC99CloseResponse("test-id", CloseReport::Untracked) == "99;i=test-id:p=close;untracked");
}

TEST_CASE("DesktopNotification.PendingChunksAreBounded", "[DesktopNotification]")
{
    // An application starting chunked notifications under ever new identifiers, and finishing none.
    auto term = NotificationTerm {};
    auto const limit = NotificationLimits.pending;
    auto const sent = limit + Overshoot;
    for (auto const n: std::views::iota(size_t { 0 }, sent))
        term.osc99(std::format("i=p{}:d=0;part{}", n, n));

    auto const pending = term.manager().pendingNotifications();
    CHECK(pending.size() == limit);
    CHECK(holds(pending, std::format("p{}", sent - 1)));
    CHECK(holds(pending, std::format("p{}", sent - limit)));
    CHECK_FALSE(holds(pending, std::format("p{}", sent - limit - 1)));

    SECTION("the newest still completes")
    {
        term.osc99(std::format("i=p{}; end", sent - 1));
        REQUIRE(term.shown().size() == 1);
        CHECK(term.shown().back().title == std::format("part{} end", sent - 1));
        CHECK(term.manager().pendingNotifications().size() == limit - 1);
    }

    SECTION("a chunk added to the oldest keeps it")
    {
        auto const oldest = std::format("p{}", sent - limit);
        term.osc99(std::format("i={}:d=0; more", oldest));
        term.osc99(std::format("i=p{}:d=0;part", sent));

        auto const after = term.manager().pendingNotifications();
        CHECK(after.size() == limit);
        CHECK(holds(after, oldest));
        CHECK_FALSE(holds(after, std::format("p{}", sent - limit + 1)));

        term.osc99(std::format("i={};", oldest));
        REQUIRE(term.shown().size() == 1);
        CHECK(term.shown().back().title == std::format("part{} more", sent - limit));
    }
}

TEST_CASE("DesktopNotification.LiveIdentifiersAreBounded", "[DesktopNotification]")
{
    // An application raising notifications under ever new identifiers, none of whose closes is observed.
    auto term = NotificationTerm {};
    auto const limit = NotificationLimits.live;

    SECTION("p=alive reports the newest ones")
    {
        auto const sent = limit + Overshoot;
        for (auto const n: std::views::iota(size_t { 0 }, sent))
            term.osc99(std::format("i=a{};hello", n));

        CHECK(term.manager().activeNotifications().size() == limit);
        auto const alive = term.aliveIdentifiers();
        CHECK(alive.size() == limit);
        CHECK(holds(alive, std::format("a{}", sent - 1)));
        CHECK(holds(alive, std::format("a{}", sent - limit)));
        CHECK_FALSE(holds(alive, std::format("a{}", sent - limit - 1)));
    }

    SECTION("raising an identifier again keeps it")
    {
        for (auto const n: std::views::iota(size_t { 0 }, limit))
            term.osc99(std::format("i=a{};hello", n));
        term.osc99("i=a0;again");
        term.osc99(std::format("i=a{};hello", limit));

        auto const alive = term.aliveIdentifiers();
        CHECK(alive.size() == limit);
        CHECK(holds(alive, "a0"));
        CHECK_FALSE(holds(alive, "a1"));
        CHECK(holds(alive, std::format("a{}", limit)));
    }
}

TEST_CASE("DesktopNotification.ChunkedTextIsBounded", "[DesktopNotification]")
{
    // An application sending a title and a body in more chunks than the bound holds. Neither bound is a
    // whole number of the code points sent, so the cut has to back off to keep the last one whole: a
    // title of three-byte euro signs, and a body of two-byte e-acutes behind one ASCII byte.
    auto term = NotificationTerm {};
    auto const cap = NotificationLimits.textBytes;
    auto const repeated = [](std::string_view codePoint, int count) {
        auto text = std::string {};
        for ([[maybe_unused]] auto const n: std::views::iota(0, count))
            text += codePoint;
        return text;
    };
    auto const euros = repeated("\xE2\x82\xAC", 100);
    auto const acutes = repeated("\xC3\xA9", 150);

    term.osc99("i=long:d=0:p=body;a");
    for ([[maybe_unused]] auto const chunk: std::views::iota(size_t { 0 }, (cap / euros.size()) + 3))
    {
        term.osc99(std::format("i=long:d=0:p=title;{}", euros));
        term.osc99(std::format("i=long:d=0:p=body;{}", acutes));
    }
    term.osc99("i=long:p=body;");

    REQUIRE(term.shown().size() == 1);
    auto const& shown = term.shown().back();
    CHECK(shown.title.size() == cap - (cap % 3));
    CHECK(shown.title.ends_with("\xE2\x82\xAC"));
    CHECK(shown.body.size() == cap - 1);
    CHECK(shown.body.starts_with("a\xC3\xA9"));
    CHECK(shown.body.ends_with("\xC3\xA9"));
    CHECK(term.manager().pendingNotifications().empty());
}
