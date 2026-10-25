// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/window/NewDefaultsNotice.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
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
        auto in = std::ifstream { configDir / "settings.yml" };
        auto content = std::ostringstream {};
        content << in.rdbuf();
        return std::move(content).str();
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
