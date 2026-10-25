// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/window/NewDefaultsNotice.hpp>

#include <core/Utils.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <QtTest/QSignalSpy>

using contour::config::DefaultsEdition;
using contour::config::DefaultsEditionSources;
using contour::window::NewDefaultsNotice;

namespace
{
/// A configuration directory, the Config loaded from it, and a reload that switches it to the newest
/// edition as settings.yml would.
struct NoticeFixture
{
    QTemporaryDir dir;
    std::filesystem::path configDir = std::filesystem::path(dir.path().toStdU16String());
    contour::config::Config config = contour::config::Config { .defaultsEdition = DefaultsEdition::Classic };
    int reloads = 0;
    std::expected<void, std::string> reloadResult {};

    /// @return A notice over this directory's files, reading `config`.
    [[nodiscard]] std::unique_ptr<NewDefaultsNotice> makeNotice()
    {
        return std::make_unique<NewDefaultsNotice>(
            [this]() -> contour::config::Config const& { return config; },
            std::make_shared<contour::config::FileGuiConfigStore>(configDir),
            std::make_shared<contour::config::FileGuiStateStore>(configDir),
            [this]() -> std::expected<void, std::string> {
                ++reloads;
                if (reloadResult)
                    config.defaultsEditionSources = DefaultsEditionSources {
                        .mainFile = config.defaultsEditionSources.mainFile,
                        .settingsFile = DefaultsEdition::Native2026,
                    };
                return reloadResult;
            });
    }

    /// @return What settings.yml holds.
    [[nodiscard]] std::string settingsFile() const
    {
        return core::readFileAsString(configDir / "settings.yml");
    }
};
} // namespace

TEST_CASE("NewDefaultsNotice: Use New writes settings.yml and reloads", "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    auto notice = fixture.makeNotice();
    REQUIRE(notice->offered());

    auto const spy = QSignalSpy { notice.get(), &NewDefaultsNotice::offeredChanged };
    notice->useNew();
    CHECK(fixture.settingsFile().contains("defaults_edition: native2026"));
    CHECK(fixture.reloads == 1);
    notice->refresh();
    CHECK_FALSE(notice->offered());
    CHECK(spy.count() == 1);
}

TEST_CASE("NewDefaultsNotice: Keep Mine is remembered", "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    auto notice = fixture.makeNotice();
    REQUIRE(notice->offered());
    notice->keepMine();
    CHECK_FALSE(notice->offered());
    CHECK_FALSE(fixture.makeNotice()->offered());
    CHECK(fixture.reloads == 0);
}

TEST_CASE("NewDefaultsNotice: a Reset Config is offered again after Keep Mine", "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    auto notice = fixture.makeNotice();
    notice->keepMine();
    REQUIRE_FALSE(notice->offered());

    // A reset contour.yml states the newest edition, but the settings.yml before it keeps classic.
    fixture.config.defaultsEditionSources =
        DefaultsEditionSources { .mainFile = DefaultsEdition::Native2026,
                                 .settingsFile = DefaultsEdition::Classic };
    notice->refresh();
    CHECK(notice->offered());
    notice->keepMine();
    CHECK_FALSE(notice->offered());
    CHECK_FALSE(fixture.makeNotice()->offered());
}

TEST_CASE("NewDefaultsNotice: a failed reload is returned and leaves the offer", "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    fixture.reloadResult = std::unexpected { std::string { "broken" } };
    auto notice = fixture.makeNotice();
    auto const switched = notice->switchToNewDefaults();
    REQUIRE_FALSE(switched.has_value());
    CHECK(switched.error() == "broken");
    CHECK(notice->offered());
}

TEST_CASE("NewDefaultsNotice: an explicit classic is never offered", "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    fixture.config.defaultsEditionSources =
        DefaultsEditionSources { .mainFile = DefaultsEdition::Classic, .settingsFile = std::nullopt };
    CHECK_FALSE(fixture.makeNotice()->offered());
}

TEST_CASE("NewDefaultsNotice: Use New keeps the other settings.yml keys", "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    fixture.config.guiManagedSettings.globalOverrides["reflow_on_resize"] = "false";
    auto notice = fixture.makeNotice();
    REQUIRE(notice->switchToNewDefaults().has_value());
    auto const written = fixture.settingsFile();
    INFO(written);
    CHECK(written.contains("defaults_edition: native2026"));
    CHECK(written.contains("reflow_on_resize: false"));
}

TEST_CASE("NewDefaultsNotice: nothing is offered under gui_config_locked", "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    fixture.config.guiConfigLocked = true;
    auto notice = fixture.makeNotice();
    CHECK_FALSE(notice->offered());
    notice->refresh();
    CHECK_FALSE(notice->offered());
    CHECK_FALSE(notice->switchToNewDefaults().has_value());
    CHECK_FALSE(std::filesystem::exists(fixture.configDir / "settings.yml"));
    CHECK(fixture.reloads == 0);

    // Unlocked by a reload, the offer comes back.
    fixture.config.guiConfigLocked = false;
    notice->refresh();
    CHECK(notice->offered());
}

TEST_CASE("NewDefaultsNotice: nothing is offered over a settings.yml that does not parse",
          "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    fixture.config.guiSettingsFileHealth = contour::config::GuiSettingsFileHealth::Unparsable;
    auto notice = fixture.makeNotice();
    CHECK_FALSE(notice->offered());
    notice->refresh();
    CHECK_FALSE(notice->offered());
    CHECK_FALSE(notice->switchToNewDefaults().has_value());
    CHECK_FALSE(std::filesystem::exists(fixture.configDir / "settings.yml"));
}

namespace
{
/// A GUI state store that can neither be read nor written.
class BrokenGuiStateStore final: public contour::config::GuiStateStore
{
  public:
    [[nodiscard]] std::expected<contour::config::GuiState, contour::config::GuiStateError> load()
        const override
    {
        return std::unexpected { contour::config::GuiStateError::Unreadable };
    }
    [[nodiscard]] std::expected<void, contour::config::GuiStateError> save(
        contour::config::GuiState const& /*state*/) override
    {
        ++saves;
        return std::unexpected { contour::config::GuiStateError::Unwritable };
    }

    int saves = 0; ///< How many saves were attempted.
};
} // namespace

TEST_CASE("NewDefaultsNotice: Keep Mine over a broken state store still hides the banner",
          "[contour][newdefaults]")
{
    auto fixture = NoticeFixture {};
    REQUIRE(fixture.dir.isValid());
    auto const store = std::make_shared<BrokenGuiStateStore>();
    auto notice = NewDefaultsNotice {
        [&fixture]() -> contour::config::Config const& { return fixture.config; },
        std::make_shared<contour::config::FileGuiConfigStore>(fixture.configDir),
        store,
        [] { return std::expected<void, std::string> {}; },
    };
    REQUIRE(notice.offered());
    notice.keepMine();
    CHECK(store->saves == 1);
    CHECK_FALSE(notice.offered());
    // The store still cannot be read: the dismissal held in memory stands.
    notice.refresh();
    CHECK_FALSE(notice.offered());
}

TEST_CASE("Config: a settings.yml that does not parse is recorded", "[config][newdefaults]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto const configDir = std::filesystem::path(dir.path().toStdU16String());
    {
        auto out = std::ofstream { configDir / "contour.yml" };
        out << "profiles:\n    main:\n        shell: /bin/sh\n";
    }
    CHECK(contour::config::loadConfigFromFile(configDir / "contour.yml").guiSettingsFileHealth
          == contour::config::GuiSettingsFileHealth::Usable);
    {
        auto out = std::ofstream { configDir / "settings.yml" };
        out << "defaults_edition: [unterminated\n";
    }
    CHECK(contour::config::loadConfigFromFile(configDir / "contour.yml").guiSettingsFileHealth
          == contour::config::GuiSettingsFileHealth::Unparsable);
}
