// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/PortalGlobalHotkey.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using contour::config::HotkeyChord;
using contour::platform::HotkeyError;
using contour::platform::HotkeyResult;
using contour::platform::IGlobalShortcutsChannel;
using contour::platform::PortalGlobalHotkey;
using contour::platform::PortalResponse;
using contour::platform::SessionCreated;
using contour::platform::ShortcutActivated;
using contour::platform::ShortcutBinding;
using contour::platform::ShortcutsBound;
using vtbackend::Key;
using vtbackend::Modifier;

namespace
{

constexpr auto Handle = "/org/freedesktop/portal/desktop/session/test/1";

/// A channel that records what it is asked and answers when the test says so.
struct FakeChannel final: IGlobalShortcutsChannel
{
    int createSessions = 0;
    std::vector<std::vector<ShortcutBinding>> binds {};
    std::vector<std::string> closed {};
    SessionCreated pendingCreate {};
    ShortcutsBound pendingBind {};
    ShortcutActivated activated {};

    void createSession(SessionCreated onCreated) override
    {
        ++createSessions;
        pendingCreate = std::move(onCreated);
    }

    void bindShortcuts(std::string const& /*handle*/,
                       std::vector<ShortcutBinding> const& shortcuts,
                       ShortcutsBound onBound) override
    {
        binds.push_back(shortcuts);
        pendingBind = std::move(onBound);
    }

    void closeSession(std::string const& handle) override { closed.push_back(handle); }

    void onActivated(ShortcutActivated callback) override { activated = std::move(callback); }

    /// Answers the outstanding CreateSession.
    void answerCreate(PortalResponse response = PortalResponse::Success)
    {
        auto answer = std::exchange(pendingCreate, nullptr);
        REQUIRE(answer);
        answer(response, response == PortalResponse::Success ? Handle : "");
    }

    /// Answers the outstanding BindShortcuts.
    void answerBind(PortalResponse response = PortalResponse::Success)
    {
        auto answer = std::exchange(pendingBind, nullptr);
        REQUIRE(answer);
        answer(response);
    }
};

/// A PortalGlobalHotkey over a FakeChannel the test keeps a pointer to.
struct Fixture
{
    FakeChannel* channel = nullptr;
    std::unique_ptr<PortalGlobalHotkey> hotkey;
    std::vector<HotkeyResult> results {};
    std::vector<std::string> pressed {};

    Fixture()
    {
        auto owned = std::make_unique<FakeChannel>();
        channel = owned.get();
        hotkey = std::make_unique<PortalGlobalHotkey>(std::move(owned));
    }

    /// Registers @p chord with callbacks that record into this fixture.
    HotkeyResult registerChord(HotkeyChord const& chord)
    {
        return hotkey->registerHotkey(
            chord,
            [this](std::string token) { pressed.push_back(std::move(token)); },
            [this](HotkeyResult result) { results.push_back(result); });
    }
};

constexpr auto ControlGrave = HotkeyChord { .modifiers = Modifier::Control, .key = U'`' };
constexpr auto AltSpace = HotkeyChord { .modifiers = Modifier::Alt, .key = U' ' };

} // namespace

TEST_CASE("PortalGlobalHotkey.registeringCreatesOneSessionThenBinds", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    CHECK(fixture.channel->createSessions == 1);
    CHECK(fixture.channel->binds.empty());
    CHECK(fixture.results.empty()); // The portal has not answered yet.

    fixture.channel->answerCreate();
    REQUIRE(fixture.channel->binds.size() == 1);
    REQUIRE(fixture.channel->binds.front().size() == 1);
    auto const& binding = fixture.channel->binds.front().front();
    CHECK(binding.id == "quick-terminal");
    CHECK(binding.description == "Show or hide the quick terminal");
    CHECK(binding.preferredTrigger == "CTRL+grave");

    fixture.channel->answerBind();
    REQUIRE(fixture.results.size() == 1);
    CHECK(fixture.results.front().has_value());
}

TEST_CASE("PortalGlobalHotkey.aRefusedBindingIsReported", "[portalhotkey]")
{
    struct Row
    {
        PortalResponse response;
        HotkeyError expected;
    };
    for (auto const& row: { Row { .response = PortalResponse::Cancelled, .expected = HotkeyError::Declined },
                            Row { .response = PortalResponse::Other, .expected = HotkeyError::Failed } })
    {
        auto fixture = Fixture {};
        CHECK(fixture.registerChord(ControlGrave).has_value());
        fixture.channel->answerCreate();
        fixture.channel->answerBind(row.response);
        REQUIRE(fixture.results.size() == 1);
        REQUIRE_FALSE(fixture.results.front().has_value());
        CHECK(fixture.results.front().error() == row.expected);
    }
}

TEST_CASE("PortalGlobalHotkey.aFailedSessionIsReportedAndRetriedByTheNextRegistration", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.channel->answerCreate(PortalResponse::Other);
    REQUIRE(fixture.results.size() == 1);
    CHECK(fixture.results.front() == HotkeyResult { std::unexpected(HotkeyError::Failed) });
    CHECK(fixture.channel->binds.empty());

    CHECK(fixture.registerChord(AltSpace).has_value());
    CHECK(fixture.channel->createSessions == 2);
}

TEST_CASE("PortalGlobalHotkey.activationOfItsShortcutCallsOnPressed", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.channel->answerCreate();
    fixture.channel->answerBind();

    fixture.channel->activated("quick-terminal", "tok");
    fixture.channel->activated("another-shortcut", "nope");
    REQUIRE(fixture.pressed.size() == 1);
    CHECK(fixture.pressed.front() == "tok");
}

TEST_CASE("PortalGlobalHotkey.anUnchangedChordSendsNothing", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.channel->answerCreate();
    fixture.channel->answerBind();

    CHECK(fixture.registerChord(ControlGrave).has_value());
    CHECK(fixture.channel->createSessions == 1);
    CHECK(fixture.channel->binds.size() == 1);
    // The settled outcome is repeated to the new callback, so a reload sees the same answer.
    CHECK(fixture.results.size() == 2);
}

TEST_CASE("PortalGlobalHotkey.aChangedChordIsBoundOnANewSession", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.channel->answerCreate();
    fixture.channel->answerBind();

    // Not a second BindShortcuts on the open session, which a portal may keep the first trigger of: the
    // session is closed and the new chord is the first, preferred one of a new session.
    CHECK(fixture.registerChord(AltSpace).has_value());
    REQUIRE(fixture.channel->closed.size() == 1);
    CHECK(fixture.channel->closed.front() == Handle);
    CHECK(fixture.channel->createSessions == 2);
    CHECK(fixture.channel->binds.size() == 1);

    fixture.channel->answerCreate();
    REQUIRE(fixture.channel->binds.size() == 2);
    CHECK(fixture.channel->binds.back().front().preferredTrigger == "ALT+space");
    fixture.channel->answerBind();
    CHECK(fixture.results.size() == 2);
    CHECK(fixture.results.back().has_value());
}

TEST_CASE("PortalGlobalHotkey.aRefusedChordIsTriedAgainByTheNextRegistration", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.channel->answerCreate();
    fixture.channel->answerBind(PortalResponse::Cancelled);
    REQUIRE(fixture.results.size() == 1);
    REQUIRE_FALSE(fixture.results.back().has_value());

    // The same chord again goes to the portal rather than being answered with the old refusal.
    CHECK(fixture.registerChord(ControlGrave).has_value());
    CHECK(fixture.results.size() == 1);
    CHECK(fixture.channel->createSessions == 2);
    fixture.channel->answerCreate();
    REQUIRE(fixture.channel->binds.size() == 2);
    fixture.channel->answerBind();
    REQUIRE(fixture.results.size() == 2);
    CHECK(fixture.results.back().has_value());
}

TEST_CASE("PortalGlobalHotkey.aChordWhoseSessionFailedIsTriedAgain", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.channel->answerCreate(PortalResponse::Other);
    REQUIRE(fixture.results.size() == 1);

    CHECK(fixture.registerChord(ControlGrave).has_value());
    CHECK(fixture.channel->createSessions == 2);
}

TEST_CASE("PortalGlobalHotkey.aChordChangedWhileTheSessionIsBeingCreatedBindsOnlyTheNewOne", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    CHECK(fixture.registerChord(AltSpace).has_value());
    CHECK(fixture.channel->createSessions == 1);

    fixture.channel->answerCreate();
    REQUIRE(fixture.channel->binds.size() == 1);
    CHECK(fixture.channel->binds.front().front().preferredTrigger == "ALT+space");
}

TEST_CASE("PortalGlobalHotkey.aSupersededBindingAnswerIsDropped", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.channel->answerCreate();
    auto const firstAnswer = fixture.channel->pendingBind;
    CHECK(fixture.registerChord(AltSpace).has_value());

    firstAnswer(PortalResponse::Cancelled); // The answer for CTRL+grave, which is no longer wanted.
    CHECK(fixture.results.empty());
    fixture.channel->answerCreate();
    fixture.channel->answerBind();
    REQUIRE(fixture.results.size() == 1);
    CHECK(fixture.results.front().has_value());
}

TEST_CASE("PortalGlobalHotkey.unregisteringClosesTheSession", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.channel->answerCreate();
    fixture.channel->answerBind();

    fixture.hotkey->unregisterHotkey();
    REQUIRE(fixture.channel->closed.size() == 1);
    CHECK(fixture.channel->closed.front() == Handle);
    fixture.channel->activated("quick-terminal", "late");
    CHECK(fixture.pressed.empty());
}

TEST_CASE("PortalGlobalHotkey.aSessionCreatedAfterUnregisteringIsClosed", "[portalhotkey]")
{
    auto fixture = Fixture {};
    CHECK(fixture.registerChord(ControlGrave).has_value());
    fixture.hotkey->unregisterHotkey();
    CHECK(fixture.channel->closed.empty()); // Nothing is open yet.

    fixture.channel->answerCreate();
    REQUIRE(fixture.channel->closed.size() == 1);
    CHECK(fixture.channel->binds.empty());
}

TEST_CASE("PortalGlobalHotkey.aKeyWithoutAPortalSpellingIsRefused", "[portalhotkey]")
{
    auto fixture = Fixture {};
    auto const result = fixture.registerChord({ .modifiers = Modifier::Control, .key = Key::F35 });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == HotkeyError::UnsupportedKey);
    // The callback hears it too, as it does of every other refusal, so the user is told.
    REQUIRE(fixture.results.size() == 1);
    CHECK(fixture.results.front() == HotkeyResult { std::unexpected(HotkeyError::UnsupportedKey) });
    CHECK(fixture.channel->createSessions == 0);
}

TEST_CASE("PortalGlobalHotkey.theNullChannelReportsFailedAtOnce", "[portalhotkey]")
{
    auto hotkey = PortalGlobalHotkey { std::make_unique<contour::platform::NullGlobalShortcutsChannel>() };
    auto result = std::optional<HotkeyResult> {};
    CHECK(hotkey.registerHotkey(ControlGrave, {}, [&](HotkeyResult r) { result = r; }).has_value());
    REQUIRE(result.has_value());
    CHECK(*result == HotkeyResult { std::unexpected(HotkeyError::Failed) });
}
