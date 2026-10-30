// SPDX-License-Identifier: Apache-2.0
//
// The D-Bus Settings portal channel. The decode runs everywhere; every case that reaches a bus runs only on
// the private session bus scripts/run-display-tests.sh stands up (contour::test::onPrivateSessionBus()),
// against FakeSettingsPortal, and SKIPs elsewhere -- CI's "portal settings adapter" step fails on a SKIP.
#ifdef __linux__

    #include <contour/platform/DBusPortalSettingsChannel.hpp>
    #include <contour/platform/PortalAppearanceSources.hpp>
    #include <contour/test/FakeSettingsPortal.hpp>
    #include <contour/test/NotificationFixtures.hpp>

    #include <QtDBus/QDBusVariant>

    #include <catch2/catch_test_macros.hpp>

    #include <array>
    #include <chrono>
    #include <cstdint>
    #include <memory>
    #include <optional>
    #include <string>
    #include <utility>
    #include <variant>
    #include <vector>

    #include <QtTest/QTest>

using contour::platform::DBusPortalSettingsChannel;
using contour::platform::PortalColor;
using contour::platform::PortalReading;
using contour::platform::PortalSetting;
using contour::platform::PortalSettings;
using contour::platform::PortalSettingsError;
using contour::platform::PortalSettingValue;
using contour::test::FakePortalTable;
using contour::test::FakeSettingsPortal;

namespace
{

/// How long a case waits for the bus: the channel's own call timeout, which bounds any answer.
constexpr auto BusWaitMilliseconds = contour::platform::DBusCallTimeoutMilliseconds;

/// The appearance namespaces, as the appearance source asks for them.
[[nodiscard]] std::vector<std::string> appearanceNamespaces()
{
    return { std::string { contour::platform::AppearanceNamespace },
             std::string { contour::platform::GnomeWmNamespace } };
}

/// The portal of Review Focus 1: dark, the GNOME accent, high contrast, buttons on the left.
[[nodiscard]] FakePortalTable reviewFocusTable()
{
    return FakePortalTable {
        { QStringLiteral("org.freedesktop.appearance"),
          QVariantMap {
              { QStringLiteral("color-scheme"), QVariant::fromValue(1U) },
              { QStringLiteral("accent-color"), contour::test::portalColorValue(0.2078, 0.5176, 0.8941) },
              { QStringLiteral("contrast"), QVariant::fromValue(1U) } } },
        { QStringLiteral("org.gnome.desktop.wm.preferences"),
          QVariantMap { { QStringLiteral("button-layout"), QStringLiteral("close,minimize,maximize:") } } },
        { QStringLiteral("org.example.other"),
          QVariantMap { { QStringLiteral("ignored"), QVariant::fromValue(7U) } } },
    };
}

/// @return The value of @p key in @p group of @p settings, or nothing.
[[nodiscard]] std::optional<PortalSettingValue> valueOf(PortalSettings const& settings,
                                                        std::string_view group,
                                                        std::string_view key)
{
    for (auto const& setting: settings)
        if (setting.group == group && setting.key == key)
            return setting.value;
    return std::nullopt;
}

/// Asks @p channel for everything and pumps events until the reply lands, or the bus wait passes.
[[nodiscard]] std::optional<PortalReading> readAllFrom(DBusPortalSettingsChannel& channel)
{
    auto reading = std::optional<PortalReading> {};
    channel.readAll([&](PortalReading reply) { reading = std::move(reply); });
    [[maybe_unused]] auto const replied =
        QTest::qWaitFor([&] { return reading.has_value(); }, BusWaitMilliseconds);
    return reading;
}

} // namespace

TEST_CASE("portalSettingValueOf decodes what QtDBus hands over", "[portal-settings]")
{
    struct Row
    {
        char const* name;
        QVariant value;
        PortalSettingValue expected;
    };
    auto const rows = std::array {
        Row { .name = "uint", .value = QVariant::fromValue(1U), .expected = std::uint32_t { 1 } },
        Row { .name = "non-negative int", .value = QVariant::fromValue(2), .expected = std::uint32_t { 2 } },
        Row { .name = "negative int", .value = QVariant::fromValue(-1), .expected = std::monostate {} },
        Row { .name = "string",
              .value = QVariant::fromValue(QStringLiteral("appmenu:close")),
              .expected = std::string { "appmenu:close" } },
        Row { .name = "double-wrapped",
              .value = QVariant::fromValue(
                  QDBusVariant { QVariant::fromValue(QDBusVariant { QVariant::fromValue(2U) }) }),
              .expected = std::uint32_t { 2 } },
        Row { .name = "double", .value = QVariant::fromValue(0.5), .expected = std::monostate {} },
        Row { .name = "invalid", .value = QVariant {}, .expected = std::monostate {} },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(contour::platform::portalSettingValueOf(row.value) == row.expected);
    }
}

TEST_CASE("ReadAll decodes a real reply", "[portal-settings]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto const portal = FakeSettingsPortal { reviewFocusTable() };
    REQUIRE(portal.isRegistered());

    auto channel = DBusPortalSettingsChannel { appearanceNamespaces(), {} };
    auto const reading = readAllFrom(channel);
    REQUIRE(reading.has_value());
    REQUIRE(reading->has_value());
    auto const& settings = **reading;
    CHECK(valueOf(settings, "org.freedesktop.appearance", "color-scheme")
          == PortalSettingValue { std::uint32_t { 1 } });
    CHECK(valueOf(settings, "org.freedesktop.appearance", "contrast")
          == PortalSettingValue { std::uint32_t { 1 } });
    CHECK(valueOf(settings, "org.freedesktop.appearance", "accent-color")
          == PortalSettingValue { PortalColor { .red = 0.2078, .green = 0.5176, .blue = 0.8941 } });
    CHECK(valueOf(settings, "org.gnome.desktop.wm.preferences", "button-layout")
          == PortalSettingValue { std::string { "close,minimize,maximize:" } });
    // The fake serves only the namespaces asked for, as the portal does.
    CHECK_FALSE(valueOf(settings, "org.example.other", "ignored").has_value());

    // Review Focus 1: the buttons on the left, close first.
    using contour::config::WindowControlKind;
    auto const appearance = contour::platform::appearanceFromPortal(*reading, {});
    CHECK(appearance.colorScheme == vtbackend::ColorPreference::Dark);
    CHECK(appearance.accent == contour::platform::PortalFallbackAccent);
    CHECK(
        appearance.buttonLayout
        == contour::config::WindowButtonLayout {
            .leading = { WindowControlKind::Close, WindowControlKind::Minimize, WindowControlKind::Maximize },
            .trailing = {} });
}

TEST_CASE("a double-wrapped value crosses the bus", "[portal-settings]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto const portal = FakeSettingsPortal { FakePortalTable {
        { QStringLiteral("org.freedesktop.appearance"),
          QVariantMap { { QStringLiteral("color-scheme"),
                          QVariant::fromValue(QDBusVariant { QVariant::fromValue(2U) }) } } } } };
    REQUIRE(portal.isRegistered());

    auto channel = DBusPortalSettingsChannel { appearanceNamespaces(), {} };
    auto const reading = readAllFrom(channel);
    REQUIRE(reading.has_value());
    REQUIRE(reading->has_value());
    CHECK(valueOf(**reading, "org.freedesktop.appearance", "color-scheme")
          == PortalSettingValue { std::uint32_t { 2 } });
}

TEST_CASE("a malformed structure crosses the bus as no value", "[portal-settings]")
{
    // An accent that is not a (ddd) is no colour: it decodes to nothing, and the reading's other keys still
    // do.
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    struct Row
    {
        char const* signature;
        QVariant value;
    };
    auto const rows = std::array {
        Row { .signature = "(dd)", .value = contour::test::portalStructureValue(0.25, 0.5) },
        Row { .signature = "(sss)",
              .value = contour::test::portalStructureValue(
                  QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")) },
    };
    for (auto const& row: rows)
    {
        INFO(row.signature);
        auto const portal = FakeSettingsPortal { FakePortalTable {
            { QStringLiteral("org.freedesktop.appearance"),
              QVariantMap { { QStringLiteral("accent-color"), row.value },
                            { QStringLiteral("color-scheme"), QVariant::fromValue(1U) } } } } };
        REQUIRE(portal.isRegistered());

        auto channel = DBusPortalSettingsChannel { appearanceNamespaces(), {} };
        auto const reading = readAllFrom(channel);
        REQUIRE(reading.has_value());
        REQUIRE(reading->has_value());
        CHECK(valueOf(**reading, "org.freedesktop.appearance", "accent-color")
              == PortalSettingValue { std::monostate {} });
        CHECK(valueOf(**reading, "org.freedesktop.appearance", "color-scheme")
              == PortalSettingValue { std::uint32_t { 1 } });
        // A portal answered, so the spec's fallback accent does not stand in for the malformed one.
        auto const appearance = contour::platform::appearanceFromPortal(*reading, {});
        CHECK_FALSE(appearance.accent.has_value());
        CHECK(appearance.colorScheme == vtbackend::ColorPreference::Dark);
    }
}

TEST_CASE("a ReadAll that times out replies Failed", "[portal-settings]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    // The fake holds its reply past the channel's call timeout.
    constexpr auto Delay = std::chrono::milliseconds { BusWaitMilliseconds * 2 };
    auto const portal = FakeSettingsPortal { reviewFocusTable(), Delay };
    REQUIRE(portal.isRegistered());

    auto channel = DBusPortalSettingsChannel { appearanceNamespaces(), {} };
    auto reading = std::optional<PortalReading> {};
    channel.readAll([&](PortalReading reply) { reading = std::move(reply); });
    REQUIRE(QTest::qWaitFor([&] { return reading.has_value(); }, BusWaitMilliseconds * 3 / 2));
    CHECK(*reading == PortalReading { std::unexpected(PortalSettingsError::Failed) });
    // Unlike NoPortal, a failure says nothing about whether a portal exists: the fallback accent stands in.
    CHECK(contour::platform::appearanceFromPortal(*reading, {}).accent
          == contour::platform::PortalFallbackAccent);
}

TEST_CASE("SettingChanged arrives for an asked namespace only", "[portal-settings]")
{
    // Review Focus 3.
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto portal = FakeSettingsPortal { reviewFocusTable() };
    REQUIRE(portal.isRegistered());

    auto changes = std::vector<PortalSetting> {};
    auto channel = DBusPortalSettingsChannel { appearanceNamespaces(), [&](PortalSetting change) {
                                                  changes.push_back(std::move(change));
                                              } };
    // A round trip first, so the match rule is installed on the bus before the signals are sent.
    REQUIRE(readAllFrom(channel).has_value());

    portal.emitSettingChanged(QStringLiteral("org.example.other"),
                              QStringLiteral("accent-color"),
                              contour::test::portalColorValue(0, 1, 0));
    portal.emitSettingChanged(QStringLiteral("org.freedesktop.appearance"),
                              QStringLiteral("accent-color"),
                              contour::test::portalColorValue(1, 0, 0));
    REQUIRE(QTest::qWaitFor([&] { return !changes.empty(); }, BusWaitMilliseconds));
    // Signals arrive in order, so the dropped one came first and is not still on its way.
    REQUIRE(changes.size() == 1);
    CHECK(changes.front()
          == PortalSetting { .group = "org.freedesktop.appearance",
                             .key = "accent-color",
                             .value = PortalColor { .red = 1, .green = 0, .blue = 0 } });
}

TEST_CASE("no portal on the bus replies NoPortal, promptly", "[portal-settings]")
{
    // Review Focus 2.
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto channel = std::unique_ptr<DBusPortalSettingsChannel> {};
    auto reading = std::optional<PortalReading> {};
    contour::test::checkReturnsWithoutWaiting([&] {
        channel = std::make_unique<DBusPortalSettingsChannel>(appearanceNamespaces(), nullptr);
        channel->readAll([&](PortalReading reply) { reading = std::move(reply); });
    });
    REQUIRE(QTest::qWaitFor([&] { return reading.has_value(); }, BusWaitMilliseconds));
    CHECK(*reading == PortalReading { std::unexpected(PortalSettingsError::NoPortal) });
}

TEST_CASE("a reply after the channel died runs nothing", "[portal-settings]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    constexpr auto Delay = std::chrono::milliseconds { 200 };
    auto const portal = FakeSettingsPortal { reviewFocusTable(), Delay };
    REQUIRE(portal.isRegistered());

    auto replies = 0;
    auto channel = std::make_unique<DBusPortalSettingsChannel>(appearanceNamespaces(), nullptr);
    channel->readAll([&](PortalReading const&) { ++replies; });
    channel.reset();
    // Long enough for the fake's reply to arrive at this process.
    QTest::qWait(static_cast<int>(Delay.count()) * 3);
    CHECK(replies == 0);
}

TEST_CASE("the real factory end to end", "[portal-settings]")
{
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto const portal = FakeSettingsPortal { reviewFocusTable() };
    REQUIRE(portal.isRegistered());

    auto received = std::vector<contour::platform::PlatformAppearance> {};
    auto const source =
        contour::platform::PortalPlatformAppearance { contour::platform::dbusPortalSettingsChannels(),
                                                      {},
                                                      [&](contour::platform::PlatformAppearance const& next) {
                                                          received.push_back(next);
                                                      } };
    REQUIRE(QTest::qWaitFor([&] { return !received.empty(); }, BusWaitMilliseconds));
    CHECK(received.size() == 1);
    CHECK(source.current().accent == contour::platform::PortalFallbackAccent);
    CHECK(source.current().colorScheme == vtbackend::ColorPreference::Dark);
}

#endif // defined(__linux__)
