// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConfigProfileCatalog.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::cli::ConfigProfileCatalog;
using contour::test::loadConfigFromYaml;

namespace
{

/// Profiles main (the default, /bin/zsh, 1000 lines) and work (/bin/fish, 2000 lines).
constexpr auto TwoProfiles = std::string_view { "default_profile: main\n"
                                                "profiles:\n"
                                                "  main:\n"
                                                "    shell: /bin/zsh\n"
                                                "    history:\n"
                                                "      limit: 1000\n"
                                                "  work:\n"
                                                "    shell: /bin/fish\n"
                                                "    escape_sandbox: false\n"
                                                "    history:\n"
                                                "      limit: 2000\n" };

} // namespace

TEST_CASE("ConfigProfileCatalog resolves named and unknown profiles", "[cli][profiles]")
{
    auto const catalog =
        ConfigProfileCatalog { loadConfigFromYaml(TwoProfiles), {}, vtbackend::ColorPreference::Light };

    auto const work = catalog.resolve("work");
    CHECK(work.match == vthost::ProfileMatch::Named);
    CHECK(work.name == "work");
    CHECK(work.shell.program == "/bin/fish");
    CHECK(work.shell.env.at("CONTOUR_PROFILE") == "work");
    CHECK(work.sandbox == vthost::SandboxEscape::Contained); // its own, not main's
    CHECK(work.settings.historyLimits.guaranteed
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(2000) });

    auto const unknown = catalog.resolve("nope");
    CHECK(unknown.match == vthost::ProfileMatch::Default);
    CHECK(unknown.name == "main");
    CHECK(unknown.shell.program == "/bin/zsh");
    CHECK(unknown.sandbox == vthost::SandboxEscape::Escapes);

    auto const unnamed = catalog.resolve("");
    CHECK(unnamed.match == vthost::ProfileMatch::Default);
    CHECK(unnamed.name == "main");
}

TEST_CASE("ConfigProfileCatalog's default is the daemon's profile when it names one", "[cli][profiles]")
{
    auto const catalog =
        ConfigProfileCatalog { loadConfigFromYaml(TwoProfiles), "work", vtbackend::ColorPreference::Light };
    auto const unknown = catalog.resolve("nope");
    CHECK(unknown.match == vthost::ProfileMatch::Default);
    CHECK(unknown.name == "work");
    CHECK(unknown.shell.program == "/bin/fish");
}

TEST_CASE("daemon.history_limit overrides the profile's history on the host", "[cli][profiles]")
{
    auto config = loadConfigFromYaml(TwoProfiles);
    config.daemon.value().historyLimit = vtbackend::LineCount(50000);
    auto const catalog = ConfigProfileCatalog { std::move(config), {}, vtbackend::ColorPreference::Light };
    auto const work = catalog.resolve("work");
    CHECK(work.settings.historyLimits.guaranteed
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(50000) });
    CHECK(work.settings.historyLimits.capacity
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(50000) });
}
