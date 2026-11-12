// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassProtocol.hpp>
#include <contour/config/Config.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/WorkspaceLister.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QTest>
#include <vthost/KnownDaemons.hpp>
#include <vthost/testing/CountingRandom.hpp>
#include <vthost/testing/TlsDaemon.hpp>

using contour::askpass::AskpassOutcome;
using contour::remote::DaemonAddress;
using vthost::proto::SwitchDaemonKind;

namespace
{

/// The saved host the switch names.
constexpr auto SavedHostName = std::string_view { "studio" };

/// A test app that has the saved host `studio` at @p port, whose window is on screen, and whose pins are
/// kept in the app's in-memory files.
class SwitchTrustRig
{
  public:
    /// @param port Where the saved host's daemon listens.
    /// @param listingTimeout How long one listing may take, as the switch gives it.
    explicit SwitchTrustRig(std::uint16_t port, std::chrono::milliseconds listingTimeout):
        _app { std::make_unique<contour::test::MockPtySessionFactory>(),
               nullptr,
               nullptr,
               nullptr,
               contour::test::TestApp::testHostIdentity(),
               nullptr,
               contour::test::TestEnvironment::Overrides { { "XDG_STATE_HOME", "/state" } } },
        _configFile { std::filesystem::path(_configDir.path().toStdU16String()) / "contour.yml" },
        _listingTimeout { listingTimeout }
    {
        REQUIRE(_configDir.isValid());
        std::ofstream { _configFile }
            << std::format("hosts:\n  {}:\n    url: \"tls://127.0.0.1:{}\"\n", SavedHostName, port);
        _app.app().config().configFile = _configFile;
        _window.controller->bindWindow(&_osWindow);
        _osWindow.show();
        REQUIRE(QTest::qWaitForWindowExposed(&_osWindow));
        _app.app().refreshWindowAttention();
    }

    /// Asks the production lister, with the transport the switch to the saved host would use, for the
    /// daemon's workspaces.
    /// @return What the listing told, once it has; nothing if it did not within the time the test waits.
    void startListing()
    {
        auto const config = _app.app().linkConfigFor(
            DaemonAddress { .kind = SwitchDaemonKind::Host, .name = std::string { SavedHostName } },
            _window.id);
        REQUIRE(config.has_value());
        _lister = contour::remote::makeControlWorkspaceLister(_listingTimeout);
        _lister->list(
            contour::remote::ListTarget { .address = DaemonAddress { .kind = SwitchDaemonKind::Host,
                                                                     .name = std::string { SavedHostName } },
                                          .transport = config->controller.transport },
            [this](auto listed) { _listed = std::move(listed); });
    }

    /// @return The prompt the window shows, or an empty map.
    [[nodiscard]] QVariantMap shownPrompt() const
    {
        return _window->promptHost()->property("pendingPrompt").toMap();
    }

    /// Answers the prompt the window shows, as its dialog's button does.
    void answer(AskpassOutcome outcome)
    {
        auto const prompt = shownPrompt();
        REQUIRE_FALSE(prompt.isEmpty());
        QMetaObject::invokeMethod(_window->promptHost(),
                                  "answerPrompt",
                                  Q_ARG(int, prompt.value("promptId").toInt()),
                                  Q_ARG(int, static_cast<int>(outcome)),
                                  Q_ARG(QString, QString {}));
    }

    /// @return The fingerprint the app pinned for @p hostPort.
    [[nodiscard]] std::optional<std::string> pinOf(std::string const& hostPort)
    {
        auto random = vthost::testing::CountingRandom {};
        auto const known = vthost::KnownDaemons { _app.fileSystem(),
                                                  vthost::knownDaemonsPath(contour::config::configHome()),
                                                  random };
        return known.pinOf(hostPort).value();
    }

    [[nodiscard]] bool isListed() const noexcept { return _listed.has_value(); }
    [[nodiscard]] auto const& listed() const { return *_listed; }

  private:
    QTemporaryDir _configDir; ///< First, so it outlives the app that reads from it.
    contour::test::TestApp _app;
    std::filesystem::path _configFile;
    std::chrono::milliseconds _listingTimeout;
    QQuickWindow _osWindow;
    contour::test::ScopedController _window { _app.manager() };
    std::unique_ptr<contour::remote::IWorkspaceLister> _lister;
    std::optional<contour::remote::WorkspaceListing> _listed;
};

} // namespace

TEST_CASE("a switch to a saved tls host that is not pinned asks in the window being switched",
          "[contour][app][switch][askpass]")
{
    using contour::remote::ListFailureKind;
    auto const served = vthost::testing::TlsDaemonThread { 2 };
    // The 15 s of the switch's listing, shortened: what a slow answer must outlast.
    constexpr auto ListingTimeout = std::chrono::milliseconds { 300 };
    auto rig = SwitchTrustRig { served.daemon().endpoint.port, ListingTimeout };
    rig.startListing();
    // The question is the window's dialog: the first contact with the daemon, with its fingerprint.
    REQUIRE(QTest::qWaitFor([&] { return !rig.shownPrompt().isEmpty(); }, 5000));
    CHECK(rig.shownPrompt().value("target").toString().toStdString() == served.daemon().hostPort);
    CHECK(rig.shownPrompt().value("text").toString().toStdString() == served.daemon().fingerprint);
    CHECK_FALSE(rig.pinOf(served.daemon().hostPort).has_value());

    SECTION("Trust pins the daemon, however long the user takes to say it")
    {
        // Longer than the whole listing is given: the question's time is not the listing's.
        QTest::qWait(static_cast<int>(2 * ListingTimeout.count()));
        REQUIRE_FALSE(rig.isListed());
        rig.answer(AskpassOutcome::Answered);
        REQUIRE(QTest::qWaitFor([&] { return rig.isListed(); }, 5000));
        CHECK(rig.pinOf(served.daemon().hostPort) == served.daemon().fingerprint);
        // The daemon speaks no control API, so the listing ends later than the question, and not as a
        // refusal to trust.
        REQUIRE_FALSE(rig.listed().has_value());
        CHECK(rig.listed().error().kind == ListFailureKind::Failed);
        CHECK_FALSE(rig.listed().error().reason.contains("not trusted"));
    }
    SECTION("Cancel pins nothing, and the listing says the user declined")
    {
        rig.answer(AskpassOutcome::Cancelled);
        REQUIRE(QTest::qWaitFor([&] { return rig.isListed(); }, 5000));
        CHECK_FALSE(rig.pinOf(served.daemon().hostPort).has_value());
        REQUIRE_FALSE(rig.listed().has_value());
        CHECK(rig.listed().error().kind == ListFailureKind::Declined);
        CHECK(rig.listed().error().reason.contains("not trusted"));
    }
}
