// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for PortalExternalLauncher — the org.freedesktop.portal.OpenURI launcher that replaces
// Qt's openUrl(), whose portal path is a BLOCKING sessionBus().call() on the calling thread. Since
// Qt 6.10 that path is taken whether or not we are sandboxed, so a wedged portal freezes the window
// on every hyperlink click. @see issue #2075, and issue #2051 for the same bug in a call we do own.
//
// The portal call is injected, so none of this needs a portal to talk to — and running the suite
// never opens a real browser at whoever is running it. The load-bearing case is the first one: that
// openUrl() returns having sent NOTHING but the request, with no reply yet in existence.

#ifdef __linux__

    #include <contour/platform/PortalExternalLauncher.hpp>
    #include <contour/test/LauncherFixtures.hpp>
    #include <contour/test/PortalFixtures.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <memory>
    #include <string>

using contour::platform::buildOpenUriArguments;
using contour::platform::CallOutcome;
using contour::platform::LaunchError;
using contour::platform::PortalExternalLauncher;
using contour::test::RecordingExternalLauncher;
using contour::test::RecordingPortalCaller;

namespace
{

/// A launcher over a recorded portal and a recorded process spawner, with both kept reachable.
///
/// The process half is injected for the same reason the portal call is: PortalExternalLauncher's
/// refusal path really runs xdg-open, so a test that did not replace it would open a browser at
/// whoever is running the suite -- and could not observe that the fallback was taken either.
struct Fixture
{
    RecordingPortalCaller portal;
    RecordingExternalLauncher* processes = nullptr;
    std::unique_ptr<PortalExternalLauncher> launcher;

    Fixture()
    {
        auto owned = std::make_unique<RecordingExternalLauncher>();
        processes = owned.get();
        launcher = std::make_unique<PortalExternalLauncher>(portal.caller(), std::move(owned));
    }
};

[[nodiscard]] std::string uriOf(QVariantList const& arguments)
{
    return arguments.at(1).toString().toStdString();
}

} // namespace

TEST_CASE("buildOpenUriArguments carries the OpenURI signature", "[contour][launcher]")
{
    auto const arguments = buildOpenUriArguments(QUrl(QStringLiteral("https://contour-terminal.org/")));

    // (s parent_window, s uri, a{sv} options) -- three arguments, in this order, or the portal
    // rejects the message before looking at what is in them.
    REQUIRE(arguments.size() == 3);
    CHECK(arguments.at(0).toString().isEmpty()); // @see the declaration on why parent_window is empty
    CHECK(uriOf(arguments) == "https://contour-terminal.org/");
    CHECK(arguments.at(2).toMap().isEmpty());
}

TEST_CASE("buildOpenUriArguments passes a local file through as a file:// URI", "[contour][launcher]")
{
    // OpenFileManager and the $EDITOR path both arrive as file:// URLs, and the portal resolves them
    // host-side -- which is what lets a sandboxed Contour open a directory it cannot itself see.
    auto const arguments = buildOpenUriArguments(QUrl::fromLocalFile(QStringLiteral("/tmp/some dir")));

    CHECK(uriOf(arguments) == "file:///tmp/some%20dir");
}

TEST_CASE("openUrl dispatches without waiting for the portal", "[contour][launcher]")
{
    // The reason this class exists. openUrl() must return having issued the call and NOT having
    // waited for it: the reply handler is still unrun, and nothing about the outcome is known yet.
    auto fixture = Fixture {};

    auto const dispatched = fixture.launcher->openUrl(QUrl(QStringLiteral("https://contour-terminal.org/")));

    CHECK(dispatched.has_value());
    REQUIRE(fixture.portal.calls.size() == 1);
    CHECK(fixture.portal.calls.front().method == QStringLiteral("OpenURI"));
    CHECK(uriOf(fixture.portal.calls.front().arguments) == "https://contour-terminal.org/");
    CHECK(fixture.portal.calls.front().onReply); // a reply is awaited, not awaited ON
}

TEST_CASE("openUrl rejects a URL the portal could not take, without calling it", "[contour][launcher]")
{
    auto fixture = Fixture {};

    SECTION("an empty URL")
    {
        auto const dispatched = fixture.launcher->openUrl(QUrl {});

        REQUIRE_FALSE(dispatched.has_value());
        CHECK(dispatched.error() == LaunchError::InvalidUrl);
        CHECK(fixture.portal.calls.empty()); // rejected here, not by the desktop
    }

    SECTION("a malformed URL")
    {
        auto const dispatched = fixture.launcher->openUrl(QUrl(QStringLiteral("http://[::malformed")));

        REQUIRE_FALSE(dispatched.has_value());
        CHECK(dispatched.error() == LaunchError::InvalidUrl);
        CHECK(fixture.portal.calls.empty());
    }
}

TEST_CASE("a portal that accepts is the end of it", "[contour][launcher]")
{
    // Nothing further must happen on the happy path -- in particular the xdg-open fallback must not
    // run, or every opened URL would open twice.
    auto fixture = Fixture {};

    CHECK(fixture.launcher->openUrl(QUrl(QStringLiteral("https://contour-terminal.org/"))).has_value());
    REQUIRE(fixture.portal.calls.size() == 1);

    fixture.portal.completeCall(0, CallOutcome::Accepted);

    CHECK(fixture.portal.calls.size() == 1); // no second attempt
    CHECK(fixture.processes->detached.empty());
}

TEST_CASE("a portal that refuses is answered late, not returned as an error", "[contour][launcher]")
{
    // The failure arrives after openUrl() already returned success, which is exactly what the
    // "dispatches, does not complete" contract on ExternalLauncher::openUrl means. There is no way
    // to hand it back to the caller, so it must not crash and must not be mistaken for acceptance.
    auto fixture = Fixture {};

    auto const dispatched = fixture.launcher->openUrl(QUrl(QStringLiteral("https://contour-terminal.org/")));
    REQUIRE(dispatched.has_value()); // already reported as accepted for delivery

    REQUIRE(fixture.portal.calls.size() == 1);
    fixture.portal.completeCall(0, CallOutcome::Failed);

    CHECK(fixture.portal.calls.size() == 1); // the fallback is a process, not another portal call
    REQUIRE(fixture.processes->detached.size() == 1);
    CHECK(fixture.processes->detached.front().program == QStringLiteral("xdg-open"));
    CHECK(fixture.processes->detached.front().arguments
          == QStringList { QStringLiteral("https://contour-terminal.org/") });
}

#endif // defined(__linux__)
