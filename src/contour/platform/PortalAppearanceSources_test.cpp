// SPDX-License-Identifier: Apache-2.0
//
// The two portal-backed sources over a fake channel: what they ask for, what they report before and after
// a reply, and that they call back once per real change. No test here reaches a bus; the D-Bus channel
// has its own suite, DBusPortalSettingsChannel_test.cpp, which runs only on a private one.

#include <contour/platform/PortalAppearanceSources.hpp>
#include <contour/platform/PortalSettingsChannel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using contour::config::AccessibilityPreferences;
using contour::config::ContrastPreference;
using contour::config::MotionPreference;
using contour::platform::AccentColorKey;
using contour::platform::AppearanceNamespace;
using contour::platform::ColorSchemeKey;
using contour::platform::ContrastKey;
using contour::platform::DesktopLayoutInputs;
using contour::platform::PlatformAppearance;
using contour::platform::PortalAccessibilityPreferenceSource;
using contour::platform::PortalColor;
using contour::platform::PortalFallbackAccent;
using contour::platform::PortalPlatformAppearance;
using contour::platform::PortalSetting;
using contour::platform::PortalSettingChanged;
using contour::platform::PortalSettings;
using contour::platform::PortalSettingsChannelFactory;
using contour::platform::PortalSettingsError;
using contour::platform::PortalSettingsReply;
using contour::platform::PortalSettingValue;
using contour::platform::ReducedMotionKey;

namespace
{

/// What a FakePortalSettingsChannel was asked, and the handlers a test delivers through.
struct FakeChannelRecord
{
    int built = 0;                          ///< How many channels the factory built.
    int destroyed = 0;                      ///< How many of them were destroyed.
    int readAllCalls = 0;                   ///< How many readAll calls they received.
    std::vector<std::string> namespaces {}; ///< The namespaces the last channel was built for.
    PortalSettingsReply pendingReply {};    ///< The last readAll's reply handler, until delivered.
    PortalSettingChanged onChange {};       ///< The last channel's change handler.

    /// Delivers @p reading to the outstanding readAll, as the bus would.
    void reply(std::expected<PortalSettings, PortalSettingsError> reading)
    {
        REQUIRE(pendingReply);
        auto handler = std::exchange(pendingReply, nullptr);
        handler(std::move(reading));
    }

    /// Delivers a SettingChanged, as the bus would.
    void change(PortalSetting setting)
    {
        REQUIRE(onChange);
        onChange(std::move(setting));
    }
};

/// A channel that records into a FakeChannelRecord instead of reaching a bus. On destruction it drops
/// both handlers, as the real channel's destruction does: nothing it held may run afterwards.
class FakePortalSettingsChannel final: public contour::platform::IPortalSettingsChannel
{
  public:
    FakePortalSettingsChannel(FakeChannelRecord& record,
                              std::vector<std::string> namespaces,
                              PortalSettingChanged onChange):
        _record { record }
    {
        ++_record.built;
        _record.namespaces = std::move(namespaces);
        _record.onChange = std::move(onChange);
    }

    FakePortalSettingsChannel(FakePortalSettingsChannel const&) = delete;
    FakePortalSettingsChannel& operator=(FakePortalSettingsChannel const&) = delete;
    FakePortalSettingsChannel(FakePortalSettingsChannel&&) = delete;
    FakePortalSettingsChannel& operator=(FakePortalSettingsChannel&&) = delete;

    ~FakePortalSettingsChannel() override
    {
        ++_record.destroyed;
        _record.pendingReply = nullptr;
        _record.onChange = nullptr;
    }

    void readAll(PortalSettingsReply onReply) override
    {
        ++_record.readAllCalls;
        _record.pendingReply = std::move(onReply);
    }

  private:
    FakeChannelRecord& _record;
};

/// @return A factory building FakePortalSettingsChannels that record into @p record.
[[nodiscard]] PortalSettingsChannelFactory fakeChannels(FakeChannelRecord& record)
{
    return [&record](std::vector<std::string> namespaces, PortalSettingChanged onChange) {
        return std::make_unique<FakePortalSettingsChannel>(
            record, std::move(namespaces), std::move(onChange));
    };
}

/// @return The setting @p key of the appearance namespace, holding @p value.
[[nodiscard]] PortalSetting appearanceSetting(std::string_view key, PortalSettingValue value)
{
    return PortalSetting { .group = std::string { AppearanceNamespace },
                           .key = std::string { key },
                           .value = std::move(value) };
}

constexpr auto Red = PortalColor { .red = 1, .green = 0, .blue = 0 };
constexpr auto Blue = PortalColor { .red = 0, .green = 0, .blue = 1 };

} // namespace

TEST_CASE("the appearance source asks for its namespaces once", "[portal-source]")
{
    auto record = FakeChannelRecord {};
    auto const source = PortalPlatformAppearance { fakeChannels(record), {}, {} };
    CHECK(record.built == 1);
    CHECK(record.readAllCalls == 1);
    CHECK(record.namespaces
          == std::vector<std::string> { "org.freedesktop.appearance", "org.gnome.desktop.wm.preferences" });
}

TEST_CASE("before any reply the appearance source publishes nothing and calls nothing", "[portal-source]")
{
    // Ruling Q4: until a portal is known to be missing, the platform theme's highlight stays.
    auto record = FakeChannelRecord {};
    auto calls = 0;
    auto const source =
        PortalPlatformAppearance { fakeChannels(record), {}, [&](PlatformAppearance const&) { ++calls; } };
    CHECK_FALSE(source.current().accent.has_value());
    CHECK_FALSE(source.current().colorScheme.has_value());
    // The layout is the files' until the portal says otherwise: here, none, so the fallback.
    CHECK(source.current().buttonLayout == contour::config::fallbackButtonLayout());
    CHECK(calls == 0);
}

TEST_CASE("with no portal the appearance source falls back to the spec's accent", "[portal-source]")
{
    // Review Focus 2 and ruling Q4.
    SECTION("a channel that replies NoPortal later calls once")
    {
        auto record = FakeChannelRecord {};
        auto received = std::vector<PlatformAppearance> {};
        auto const source =
            PortalPlatformAppearance { fakeChannels(record), {}, [&](PlatformAppearance const& next) {
                                          received.push_back(next);
                                      } };
        record.reply(std::unexpected(PortalSettingsError::NoPortal));
        CHECK(source.current().accent == PortalFallbackAccent);
        REQUIRE(received.size() == 1);
        CHECK(received.front() == source.current());
    }

    SECTION("the null channel answers inside the constructor, which is the starting state, not a change")
    {
        auto calls = 0;
        auto const source = PortalPlatformAppearance { contour::platform::nullPortalSettingsChannels(),
                                                       {},
                                                       [&](PlatformAppearance const&) { ++calls; } };
        CHECK(source.current().accent == PortalFallbackAccent);
        CHECK_FALSE(source.current().colorScheme.has_value());
        CHECK(source.current().buttonLayout == contour::config::fallbackButtonLayout());
        CHECK(calls == 0);
    }

    SECTION("GTK's settings.ini still places the buttons")
    {
        using contour::config::WindowControlKind;
        auto const files =
            contour::config::ButtonLayoutSources { .gnomeButtonLayout = std::nullopt,
                                                   .kwin = std::nullopt,
                                                   .gtkDecorationLayout = "close,minimize,maximize:" };
        auto const source = PortalPlatformAppearance {
            contour::platform::nullPortalSettingsChannels(),
            DesktopLayoutInputs { .files = files, .desktop = contour::config::HostPlatform::Other },
            {}
        };
        CHECK(source.current().buttonLayout
              == contour::config::WindowButtonLayout { .leading = { WindowControlKind::Close,
                                                                    WindowControlKind::Minimize,
                                                                    WindowControlKind::Maximize },
                                                       .trailing = {} });
    }
}

TEST_CASE("a live button-layout change moves the buttons", "[portal-source]")
{
    using contour::config::WindowControlKind;
    auto record = FakeChannelRecord {};
    auto received = std::vector<PlatformAppearance> {};
    auto const source =
        PortalPlatformAppearance { fakeChannels(record), {}, [&](PlatformAppearance const& next) {
                                      received.push_back(next);
                                  } };
    record.reply(PortalSettings {});
    record.change(PortalSetting { .group = std::string { contour::platform::GnomeWmNamespace },
                                  .key = "button-layout",
                                  .value = std::string { "close:" } });
    REQUIRE(received.size() == 1);
    CHECK(received.back().buttonLayout
          == contour::config::WindowButtonLayout { .leading = { WindowControlKind::Close }, .trailing = {} });
}

TEST_CASE("a reply that changes the look calls once", "[portal-source]")
{
    auto record = FakeChannelRecord {};
    auto received = std::vector<PlatformAppearance> {};
    auto const source =
        PortalPlatformAppearance { fakeChannels(record), {}, [&](PlatformAppearance const& next) {
                                      received.push_back(next);
                                  } };
    record.reply(PortalSettings { appearanceSetting(AccentColorKey, Red) });
    CHECK(source.current().accent == vtbackend::RGBColor { 0xFF, 0, 0 });
    REQUIRE(received.size() == 1);
    CHECK(received.front().accent == vtbackend::RGBColor { 0xFF, 0, 0 });
}

TEST_CASE("a reply that changes nothing calls nothing", "[portal-source]")
{
    // A portal that answers without any key this source reads leaves the starting appearance.
    auto record = FakeChannelRecord {};
    auto calls = 0;
    auto const source =
        PortalPlatformAppearance { fakeChannels(record), {}, [&](PlatformAppearance const&) { ++calls; } };
    record.reply(PortalSettings {});
    CHECK(calls == 0);
}

TEST_CASE("live appearance changes call once per real change", "[portal-source]")
{
    // Review Focus 3.
    auto record = FakeChannelRecord {};
    auto received = std::vector<PlatformAppearance> {};
    auto const source =
        PortalPlatformAppearance { fakeChannels(record), {}, [&](PlatformAppearance const& next) {
                                      received.push_back(next);
                                  } };
    record.reply(PortalSettings { appearanceSetting(AccentColorKey, Red) });
    received.clear();

    record.change(appearanceSetting(AccentColorKey, Blue));
    REQUIRE(received.size() == 1);
    CHECK(received.back().accent == vtbackend::RGBColor { 0, 0, 0xFF });

    record.change(appearanceSetting(AccentColorKey, Blue));
    CHECK(received.size() == 1);

    record.change(appearanceSetting(ColorSchemeKey, std::uint32_t { 1 }));
    REQUIRE(received.size() == 2);
    CHECK(received.back().colorScheme == vtbackend::ColorPreference::Dark);
    CHECK(source.current().colorScheme == vtbackend::ColorPreference::Dark);

    record.change(appearanceSetting(ContrastKey, std::uint32_t { 1 }));
    record.change(PortalSetting { .group = "org.example.other", .key = "accent-color", .value = Red });
    CHECK(received.size() == 2);
}

TEST_CASE("a change after a NoPortal reply means a portal answers after all", "[portal-source]")
{
    auto record = FakeChannelRecord {};
    auto received = std::vector<PlatformAppearance> {};
    auto const source =
        PortalPlatformAppearance { fakeChannels(record), {}, [&](PlatformAppearance const& next) {
                                      received.push_back(next);
                                  } };
    record.reply(std::unexpected(PortalSettingsError::Failed));
    REQUIRE(source.current().accent == PortalFallbackAccent);

    record.change(appearanceSetting(ColorSchemeKey, std::uint32_t { 2 }));
    CHECK_FALSE(source.current().accent.has_value());
    CHECK(source.current().colorScheme == vtbackend::ColorPreference::Light);
    CHECK(received.size() == 2);
}

// The cache's channel handlers capture its address, so it neither copies nor moves.
static_assert(!std::is_copy_constructible_v<contour::platform::PortalReadingCache>);
static_assert(!std::is_move_constructible_v<contour::platform::PortalReadingCache>);
static_assert(!std::is_copy_assignable_v<contour::platform::PortalReadingCache>);
static_assert(!std::is_move_assignable_v<contour::platform::PortalReadingCache>);

TEST_CASE("a failed reply after a change keeps the settings the change brought", "[portal-source]")
{
    // The change proved a portal answers; a ReadAll that timed out afterwards says nothing about what it
    // said.
    auto record = FakeChannelRecord {};
    auto received = std::vector<PlatformAppearance> {};
    auto const source =
        PortalPlatformAppearance { fakeChannels(record), {}, [&](PlatformAppearance const& next) {
                                      received.push_back(next);
                                  } };
    record.change(appearanceSetting(AccentColorKey, Blue));
    REQUIRE(source.current().accent == vtbackend::RGBColor { 0, 0, 0xFF });
    REQUIRE(received.size() == 1);

    record.reply(std::unexpected(PortalSettingsError::Failed));
    CHECK(source.current().accent == vtbackend::RGBColor { 0, 0, 0xFF });
    CHECK(received.size() == 1);
}

TEST_CASE("a reply after the appearance source died runs nothing", "[portal-source]")
{
    auto record = FakeChannelRecord {};
    auto calls = 0;
    auto source = std::make_unique<PortalPlatformAppearance>(
        fakeChannels(record), DesktopLayoutInputs {}, [&](PlatformAppearance const&) { ++calls; });
    source.reset();
    // The channel died with its source, and with it both handlers: there is nothing left to deliver to.
    CHECK(record.destroyed == 1);
    CHECK_FALSE(record.pendingReply);
    CHECK_FALSE(record.onChange);
    CHECK(calls == 0);
}

TEST_CASE("the accessibility source reads the portal's contrast and motion", "[portal-source]")
{
    auto record = FakeChannelRecord {};
    auto received = std::vector<AccessibilityPreferences> {};
    auto const source =
        PortalAccessibilityPreferenceSource { fakeChannels(record), [&](AccessibilityPreferences next) {
                                                 received.push_back(next);
                                             } };
    CHECK(record.namespaces == std::vector<std::string> { "org.freedesktop.appearance" });
    CHECK(record.readAllCalls == 1);
    CHECK(source.current() == AccessibilityPreferences {});

    record.reply(PortalSettings { appearanceSetting(ContrastKey, std::uint32_t { 1 }) });
    REQUIRE(received.size() == 1);
    CHECK(received.back().contrast == ContrastPreference::Increased);

    record.change(appearanceSetting(ReducedMotionKey, std::uint32_t { 1 }));
    REQUIRE(received.size() == 2);
    CHECK(received.back().motion == MotionPreference::Reduced);
    CHECK(source.current().contrast == ContrastPreference::Increased);

    record.change(appearanceSetting(ReducedMotionKey, std::uint32_t { 1 }));
    record.change(appearanceSetting(AccentColorKey, Red));
    CHECK(received.size() == 2);
}

TEST_CASE("with no portal the accessibility source keeps the defaults", "[portal-source]")
{
    auto calls = 0;
    auto const source = PortalAccessibilityPreferenceSource { contour::platform::nullPortalSettingsChannels(),
                                                              [&](AccessibilityPreferences) { ++calls; } };
    CHECK(source.current() == AccessibilityPreferences {});
    CHECK(calls == 0);
}
