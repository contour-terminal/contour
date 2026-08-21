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

    #include <catch2/catch_test_macros.hpp>

    #include <functional>
    #include <string>
    #include <utility>
    #include <vector>

using contour::platform::buildOpenUriArguments;
using contour::platform::CallOutcome;
using contour::platform::LaunchError;
using contour::platform::PortalExternalLauncher;

namespace
{

/// One recorded portal method call, with the reply the test has yet to deliver.
struct RecordedCall
{
    QString method;
    QVariantList arguments;
    std::function<void(CallOutcome)> onReply;
};

/// Everything the launcher reached for, and nothing it was given back unasked.
///
/// Deliberately not a mock framework: the recorded vector IS the assertion. Nothing replies on its
/// own, which is how a test observes that openUrl() returns before any reply exists -- the whole
/// property this class was written for.
struct RecordingPortal
{
    std::vector<RecordedCall> calls;

    [[nodiscard]] contour::platform::PortalCaller caller()
    {
        return [this](QObject* /*context*/,
                      QLatin1StringView method,
                      QVariantList const& arguments,
                      std::function<void(CallOutcome)> onReply) {
            calls.push_back({ QString(method), arguments, std::move(onReply) });
        };
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
    auto portal = RecordingPortal {};
    auto launcher = PortalExternalLauncher { portal.caller() };

    auto const dispatched = launcher.openUrl(QUrl(QStringLiteral("https://contour-terminal.org/")));

    CHECK(dispatched.has_value());
    REQUIRE(portal.calls.size() == 1);
    CHECK(portal.calls.front().method == QStringLiteral("OpenURI"));
    CHECK(uriOf(portal.calls.front().arguments) == "https://contour-terminal.org/");
    CHECK(portal.calls.front().onReply); // a reply is awaited, not awaited ON
}

TEST_CASE("openUrl rejects a URL the portal could not take, without calling it", "[contour][launcher]")
{
    auto portal = RecordingPortal {};
    auto launcher = PortalExternalLauncher { portal.caller() };

    SECTION("an empty URL")
    {
        auto const dispatched = launcher.openUrl(QUrl {});

        REQUIRE_FALSE(dispatched.has_value());
        CHECK(dispatched.error() == LaunchError::InvalidUrl);
        CHECK(portal.calls.empty()); // rejected here, not by the desktop
    }

    SECTION("a malformed URL")
    {
        auto const dispatched = launcher.openUrl(QUrl(QStringLiteral("http://[::malformed")));

        REQUIRE_FALSE(dispatched.has_value());
        CHECK(dispatched.error() == LaunchError::InvalidUrl);
        CHECK(portal.calls.empty());
    }
}

TEST_CASE("openUrl reports DispatchFailed when there is no caller at all", "[contour][launcher]")
{
    // A launcher built with an empty PortalCaller has nothing to send through. It answers rather
    // than calling an empty std::function, which would be undefined behaviour.
    auto launcher = PortalExternalLauncher { contour::platform::PortalCaller {} };

    auto const dispatched = launcher.openUrl(QUrl(QStringLiteral("https://contour-terminal.org/")));

    REQUIRE_FALSE(dispatched.has_value());
    CHECK(dispatched.error() == LaunchError::DispatchFailed);
}

TEST_CASE("a portal that accepts is the end of it", "[contour][launcher]")
{
    // Nothing further must happen on the happy path -- in particular the xdg-open fallback must not
    // run, or every opened URL would open twice.
    auto portal = RecordingPortal {};
    auto launcher = PortalExternalLauncher { portal.caller() };

    CHECK(launcher.openUrl(QUrl(QStringLiteral("https://contour-terminal.org/"))).has_value());
    REQUIRE(portal.calls.size() == 1);

    portal.calls.front().onReply(CallOutcome::Accepted);

    CHECK(portal.calls.size() == 1); // no second attempt
}

TEST_CASE("a portal that refuses is answered late, not returned as an error", "[contour][launcher]")
{
    // The failure arrives after openUrl() already returned success, which is exactly what the
    // "dispatches, does not complete" contract on ExternalLauncher::openUrl means. There is no way
    // to hand it back to the caller, so it must not crash and must not be mistaken for acceptance.
    auto portal = RecordingPortal {};
    auto launcher = PortalExternalLauncher { portal.caller() };

    auto const dispatched = launcher.openUrl(QUrl(QStringLiteral("https://contour-terminal.org/")));
    REQUIRE(dispatched.has_value()); // already reported as accepted for delivery

    REQUIRE(portal.calls.size() == 1);
    portal.calls.front().onReply(CallOutcome::Failed); // reaches the xdg-open fallback

    CHECK(portal.calls.size() == 1); // the fallback is a process, not another portal call
}

#endif // defined(__linux__)
