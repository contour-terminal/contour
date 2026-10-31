// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ConfigFwd.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/session/PreviewTranscript.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/SettingsFixtures.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsPreview.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <QtTest/QSignalSpy>

using namespace contour;

namespace
{

constexpr auto PreviewConfigYaml = std::string_view { R"(
default_profile: main
profiles:
    main:
        show_title_bar: true
)" };

/// A Settings controller over a contour.yml with one more profile, `work`, a copy of `main` whose status line
/// is off, open for editing; and a preview of it over the app's session manager.
struct PreviewFixture
{
    test::TestApp app;
    test::ControllerFixture settings { PreviewConfigYaml, std::shared_ptr<config::GuiConfigStore> {} };
    window::SettingsPreview preview;

    PreviewFixture():
        preview { *settings.controller, [this](config::Config const& cfg, std::string const& name) {
                     return app.manager().createPreviewSession(cfg, name);
                 } }
    {
        auto work = *settings.cfg.findProfile("main");
        work.statusLine.value().initialType = vtbackend::StatusDisplayType::None;
        REQUIRE(
            settings.store->write(config::profileFile("work"), config::emitProfileYaml(work)).has_value());
        settings.reload();
        settings.controller->editProfile("work");
        REQUIRE_FALSE(settings.controller->editingReadOnly());
    }

    /// @return The text of the preview terminal's page.
    [[nodiscard]] std::string pageText() const
    {
        REQUIRE(preview.session() != nullptr);
        return preview.session()->terminal().primaryScreen().renderMainPageText();
    }

    /// @return The name of the scheme the open profile draws with, read off the configuration directly.
    [[nodiscard]] std::string schemeOfWork() const
    {
        auto const& colors = settings.cfg.findProfile("work")->colors.value();
        if (auto const* simple = std::get_if<config::SimpleColorConfig>(&colors))
            return simple->colorScheme;
        return std::get<config::DualColorConfig>(colors).colorSchemeDark; // the fixture's appearance is dark
    }
};

} // namespace

TEST_CASE("previewCaption names the font and the scheme, a whole size without decimals",
          "[contour][settingspreview]")
{
    CHECK(session::previewCaption("JetBrains Mono", 13.0, "Contour Dark")
          == "Live preview — JetBrains Mono 13 · Contour Dark");
    CHECK(session::previewCaption("JetBrains Mono", 12.5, "Contour Dark")
          == "Live preview — JetBrains Mono 12.5 · Contour Dark");
}

TEST_CASE("SettingsPreview: show makes a session over the transcript that is in no window",
          "[contour][gui][settingspreview]")
{
    auto fx = PreviewFixture {};
    REQUIRE(fx.preview.session() == nullptr);
    auto created = QSignalSpy { &fx.preview, &window::SettingsPreview::sessionChanged };

    fx.preview.show("work");

    REQUIRE(fx.preview.session() != nullptr);
    CHECK(created.count() == 1);
    CHECK(fx.preview.session()->role() == session::SessionRole::Preview);
    auto const text = fx.pageText();
    CHECK(text.contains("~/repo/contour ❯ git status --short"));
    CHECK(text.contains(" M src/contour/config/Config.cpp"));
    // Not the manager's: no model session, so no window, tab or layout has it.
    CHECK(fx.app.manager().sessionForId(fx.preview.session()->modelSessionId()) == nullptr);

    // A second show reuses the session.
    fx.preview.show("work");
    CHECK(created.count() == 1);
}

TEST_CASE("SettingsPreview: every change shows before it is written", "[contour][gui][settingspreview]")
{
    auto fx = PreviewFixture {};
    fx.preview.show("work");
    REQUIRE(fx.preview.session() != nullptr);
    auto const before = fx.preview.session()->profile().fonts.value().size.pt;
    REQUIRE(before != 18.0);
    auto const family = fx.settings.cfg.findProfile("work")->fonts.value().regular.familyName;
    auto captioned = QSignalSpy { &fx.preview, &window::SettingsPreview::captionChanged };

    REQUIRE(fx.settings.controller->setProfileField("font_size", 18.0).has_value());

    // The debounce has not fired: nothing is written, and the preview shows the draft.
    CHECK(fx.settings.cfg.findProfile("work")->fonts.value().size.pt == before);
    CHECK(fx.preview.session()->profile().fonts.value().size.pt == 18.0);
    CHECK(fx.preview.caption().toStdString()
          == std::format("Live preview — {} 18 · {}", family, fx.schemeOfWork()));
    CHECK(captioned.count() == 1);
}

TEST_CASE("SettingsPreview: a scheme draft shows in the configuration it previews",
          "[contour][gui][settingspreview]")
{
    auto fx = PreviewFixture {};
    REQUIRE(fx.settings.store
                ->write(config::colorSchemeFile("midnight"),
                        config::emitColorSchemeYaml(vtbackend::ColorPalette {}))
                .has_value());
    fx.settings.reload();
    fx.settings.controller->editColorScheme("midnight");
    REQUIRE(fx.settings.controller->setSchemeColor("background", "#101010").has_value());

    auto const preview = fx.settings.controller->previewConfig();

    CHECK(preview.colorschemes.value().at("midnight").defaultBackground
          == vtbackend::RGBColor { 0x10, 0x10, 0x10 });
    // The loaded configuration has not seen it: the draft is the only place the colour is.
    CHECK_FALSE(fx.settings.cfg.colorschemes.value().contains("midnight"));
}

TEST_CASE("SettingsPreview: showStatusLine forces the indicator on", "[contour][gui][settingspreview]")
{
    auto fx = PreviewFixture {};

    fx.preview.show("work");
    REQUIRE(fx.preview.session() != nullptr);
    REQUIRE(fx.preview.session()->profile().statusLine.value().initialType
            == vtbackend::StatusDisplayType::None);

    fx.preview.showStatusLine("work");
    CHECK(fx.preview.session()->profile().statusLine.value().initialType
          == vtbackend::StatusDisplayType::Indicator);
}

TEST_CASE("SettingsPreview: a status line segment edited on the page shows in the preview at once",
          "[contour][gui][settingspreview][statusline]")
{
    auto fx = PreviewFixture {};

    fx.preview.showStatusLine("work");
    REQUIRE(fx.preview.session() != nullptr);
    auto const before = fx.preview.session()->profile().statusLine.value().indicator.right;
    REQUIRE(before != "{Clock}");

    fx.settings.controller->setIndicatorSegment(2, QStringLiteral("{Clock}"));

    auto const& shown = fx.preview.session()->profile().statusLine.value();
    CHECK(shown.indicator.right == "{Clock}");
    // The page asked for the status line, so an edit of the draft does not turn it off again.
    CHECK(shown.initialType == vtbackend::StatusDisplayType::Indicator);
}

TEST_CASE("SettingsPreview: a profile that does not exist makes no session",
          "[contour][gui][settingspreview]")
{
    auto fx = PreviewFixture {};

    fx.preview.show("nonexistent");

    CHECK(fx.preview.session() == nullptr);
    CHECK(fx.preview.caption().isEmpty());
}

TEST_CASE("SettingsPreview: the controller names the profile and scheme it previews",
          "[contour][gui][settingspreview]")
{
    auto fx = PreviewFixture {};

    CHECK(fx.settings.controller->previewProfileName() == "work");
    CHECK(fx.settings.controller->previewSchemeNameOf(
              fx.settings.controller->previewConfig().profiles.value().at("work"))
          == fx.schemeOfWork());
}
