// SPDX-License-Identifier: Apache-2.0
//
// The D-Bus GlobalShortcuts channel. Every case reaches a bus, so each runs only on the private session
// bus scripts/run-display-tests.sh stands up (contour::test::onPrivateSessionBus()), against
// FakeGlobalShortcutsPortal, and SKIPs elsewhere -- CI's "portal settings adapter" step fails on a SKIP.
#ifdef __linux__

    #include <contour/platform/DBusGlobalShortcutsChannel.hpp>
    #include <contour/platform/PortalGlobalHotkey.hpp>
    #include <contour/test/FakeGlobalShortcutsPortal.hpp>
    #include <contour/test/NotificationFixtures.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <memory>
    #include <optional>
    #include <string>
    #include <utility>

    #include <QtTest/QTest>

using contour::platform::DBusGlobalShortcutsChannel;
using contour::platform::PortalResponse;
using contour::platform::ShortcutBinding;
using contour::test::FakeGlobalShortcutsPortal;

namespace
{

/// How long a case waits for the bus: the channel's own call timeout, which bounds any answer.
constexpr auto BusWaitMilliseconds = contour::platform::DBusCallTimeoutMilliseconds;

struct Created
{
    PortalResponse response = PortalResponse::Other;
    std::string handle {};
};

/// Creates a session on @p channel and pumps events until the answer lands.
[[nodiscard]] std::optional<Created> createSessionOn(DBusGlobalShortcutsChannel& channel)
{
    auto created = std::optional<Created> {};
    channel.createSession([&](PortalResponse response, std::string handle) {
        created = Created { .response = response, .handle = std::move(handle) };
    });
    [[maybe_unused]] auto const answered =
        QTest::qWaitFor([&] { return created.has_value(); }, BusWaitMilliseconds);
    return created;
}

/// Binds the quick-terminal shortcut and pumps events until the answer lands.
[[nodiscard]] std::optional<PortalResponse> bindOn(DBusGlobalShortcutsChannel& channel,
                                                   std::string const& handle)
{
    auto bound = std::optional<PortalResponse> {};
    channel.bindShortcuts(handle,
                          { ShortcutBinding { .id = "quick-terminal",
                                              .description = "Show or hide the quick terminal",
                                              .preferredTrigger = "CTRL+grave" } },
                          [&](PortalResponse response) { bound = response; });
    [[maybe_unused]] auto const answered =
        QTest::qWaitFor([&] { return bound.has_value(); }, BusWaitMilliseconds);
    return bound;
}

} // namespace

TEST_CASE("a session is created and a shortcut bound over the bus", "[globalhotkey][portal]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeGlobalShortcutsPortal {};
    REQUIRE(portal.isRegistered());

    auto channel = DBusGlobalShortcutsChannel {};
    auto const created = createSessionOn(channel);
    REQUIRE(created.has_value());
    CHECK(created->response == PortalResponse::Success);
    REQUIRE(portal.state.sessions.size() == 1);
    CHECK(created->handle == portal.state.sessions.front().toStdString());

    auto const bound = bindOn(channel, created->handle);
    REQUIRE(bound.has_value());
    CHECK(*bound == PortalResponse::Success);
    REQUIRE(portal.state.bound.size() == 1);
    auto const& shortcut = portal.state.bound.front();
    CHECK(shortcut.id == QStringLiteral("quick-terminal"));
    CHECK(shortcut.properties.value(QStringLiteral("description")).toString()
          == QStringLiteral("Show or hide the quick terminal"));
    CHECK(shortcut.properties.value(QStringLiteral("preferred_trigger")).toString()
          == QStringLiteral("CTRL+grave"));
}

TEST_CASE("a host application registers its id before the first request", "[globalhotkey][portal]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeGlobalShortcutsPortal {};
    REQUIRE(portal.isRegistered());

    auto channel = DBusGlobalShortcutsChannel { contour::platform::sessionBusSubscriber(),
                                                std::string(contour::platform::PortalApplicationId) };
    auto const created = createSessionOn(channel);
    REQUIRE(created.has_value());
    CHECK(created->response == PortalResponse::Success);
    CHECK(portal.state.registered == QStringList { QStringLiteral("org.contourterminal.Contour") });
    // Ahead of everything else the portal was asked.
    REQUIRE(portal.state.calls.size() == 2);
    CHECK(portal.state.calls.front() == QStringLiteral("Register"));
    CHECK(portal.state.calls.back() == QStringLiteral("CreateSession"));

    // Once per channel, however many sessions it makes.
    auto const second = createSessionOn(channel);
    REQUIRE(second.has_value());
    CHECK(portal.state.registered.size() == 1);
}

TEST_CASE("a sandboxed application registers nothing", "[globalhotkey][portal]")
{
    using contour::platform::HostPortalRegistrar;
    CHECK(contour::platform::registryApplicationIdFor(vtpty::SandboxState::Host,
                                                      HostPortalRegistrar::Application)
          == std::optional<std::string_view> { "org.contourterminal.Contour" });
    CHECK_FALSE(contour::platform::registryApplicationIdFor(vtpty::SandboxState::Flatpak,
                                                            HostPortalRegistrar::Application)
                    .has_value());
    // Qt's own registration (6.11 on) is the only one: ours would fail on the shared connection.
    CHECK_FALSE(
        contour::platform::registryApplicationIdFor(vtpty::SandboxState::Host, HostPortalRegistrar::Qt)
            .has_value());
    CHECK(contour::platform::QtHostPortalRegistrar
          == (QT_VERSION >= QT_VERSION_CHECK(6, 11, 0) ? HostPortalRegistrar::Qt
                                                       : HostPortalRegistrar::Application));

    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeGlobalShortcutsPortal {};
    REQUIRE(portal.isRegistered());
    auto channel = DBusGlobalShortcutsChannel {};
    REQUIRE(createSessionOn(channel).has_value());
    CHECK(portal.state.registered.isEmpty());
    CHECK(portal.state.calls == QStringList { QStringLiteral("CreateSession") });
}

TEST_CASE("a portal without a Registry is carried on with", "[globalhotkey][portal]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeGlobalShortcutsPortal {};
    REQUIRE(portal.isRegistered());
    portal.state.hasRegistry = false;

    auto channel = DBusGlobalShortcutsChannel { contour::platform::sessionBusSubscriber(),
                                                std::string(contour::platform::PortalApplicationId) };
    auto const created = createSessionOn(channel);
    REQUIRE(created.has_value());
    CHECK(created->response == PortalResponse::Success);
    CHECK(portal.state.registered.isEmpty());
}

TEST_CASE("a refusal by the portal reaches the callback", "[globalhotkey][portal]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeGlobalShortcutsPortal {};
    REQUIRE(portal.isRegistered());

    auto channel = DBusGlobalShortcutsChannel {};
    auto const created = createSessionOn(channel);
    REQUIRE(created.has_value());
    REQUIRE(created->response == PortalResponse::Success);

    portal.state.bindResponse = 1;
    auto const cancelled = bindOn(channel, created->handle);
    REQUIRE(cancelled.has_value());
    CHECK(*cancelled == PortalResponse::Cancelled);

    portal.state.bindResponse = 2;
    auto const other = bindOn(channel, created->handle);
    REQUIRE(other.has_value());
    CHECK(*other == PortalResponse::Other);
}

TEST_CASE("an Activated signal of our session reaches the callback with its token", "[globalhotkey][portal]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeGlobalShortcutsPortal {};
    REQUIRE(portal.isRegistered());

    auto channel = DBusGlobalShortcutsChannel {};
    auto activations = std::vector<std::pair<std::string, std::string>> {};
    channel.onActivated([&](std::string id, std::string token) {
        activations.emplace_back(std::move(id), std::move(token));
    });
    auto const created = createSessionOn(channel);
    REQUIRE(created.has_value());
    REQUIRE(created->response == PortalResponse::Success);

    // A session this channel never created, then ours: signals arrive in order, so the first, dropped one
    // is not still on its way when the second is seen.
    portal.emitActivated(QStringLiteral("/org/freedesktop/portal/desktop/session/other/1"),
                         QStringLiteral("quick-terminal"),
                         QStringLiteral("foreign"));
    portal.emitActivated(
        QString::fromStdString(created->handle), QStringLiteral("quick-terminal"), QStringLiteral("tok"));
    REQUIRE(QTest::qWaitFor([&] { return !activations.empty(); }, BusWaitMilliseconds));
    REQUIRE(activations.size() == 1);
    CHECK(activations.front() == std::pair<std::string, std::string> { "quick-terminal", "tok" });
}

TEST_CASE("closing a session reaches the portal", "[globalhotkey][portal]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeGlobalShortcutsPortal {};
    REQUIRE(portal.isRegistered());

    auto channel = DBusGlobalShortcutsChannel {};
    auto const created = createSessionOn(channel);
    REQUIRE(created.has_value());
    channel.closeSession(created->handle);
    REQUIRE(QTest::qWaitFor([&] { return !portal.state.closed.isEmpty(); }, BusWaitMilliseconds));
    CHECK(portal.state.closed.front().toStdString() == created->handle);
}

TEST_CASE("no portal on the bus reports Other, promptly", "[globalhotkey][portal]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto channel = std::make_unique<DBusGlobalShortcutsChannel>();
    auto created = std::optional<Created> {};
    contour::test::checkReturnsWithoutWaiting([&] {
        channel->createSession([&](PortalResponse response, std::string handle) {
            created = Created { .response = response, .handle = std::move(handle) };
        });
    });
    REQUIRE(QTest::qWaitFor([&] { return created.has_value(); }, BusWaitMilliseconds));
    CHECK(created->response == PortalResponse::Other);
}

TEST_CASE("the real hotkey end to end", "[globalhotkey][portal]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeGlobalShortcutsPortal {};
    REQUIRE(portal.isRegistered());

    auto const hotkey = contour::platform::makeGlobalHotkey();
    auto result = std::optional<contour::platform::HotkeyResult> {};
    auto pressed = std::vector<std::string> {};
    CHECK(hotkey
              ->registerHotkey(
                  { .modifiers = vtbackend::Modifier::Control, .key = U'`' },
                  [&](std::string token) { pressed.push_back(std::move(token)); },
                  [&](contour::platform::HotkeyResult settled) { result = settled; })
              .has_value());
    REQUIRE(QTest::qWaitFor([&] { return result.has_value(); }, BusWaitMilliseconds));
    CHECK(result->has_value());
    REQUIRE(portal.state.sessions.size() == 1);

    portal.emitActivated(
        portal.state.sessions.front(), QStringLiteral("quick-terminal"), QStringLiteral("t1"));
    REQUIRE(QTest::qWaitFor([&] { return !pressed.empty(); }, BusWaitMilliseconds));
    CHECK(pressed.front() == "t1");

    hotkey->unregisterHotkey();
    REQUIRE(QTest::qWaitFor([&] { return !portal.state.closed.isEmpty(); }, BusWaitMilliseconds));
}

#endif // defined(__linux__)
