// SPDX-License-Identifier: Apache-2.0
//
// A session's status-line extensions: what it builds for a profile that declares them (and that it builds
// nothing for one that does not), what a reload does to them, and the question an extension asks before
// it may run its commands.
#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/PermissionFixtures.hpp>

#include <vtpty/MockPty.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QtTest/QTest>
#include <vtextension/CommandPolicy.hpp>
#include <vtextension/ExtensionConsent.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/SessionExtensions.hpp>
#include <vtextension/testing/TestModules.hpp>

using contour::config::ExtensionConfig;
using contour::session::PermissionDecision;
using contour::session::PermissionMemory;
using contour::session::TerminalSession;
using contour::test::answerShown;
using contour::test::TestApp;
using vtextension::GrantState;

namespace
{
constexpr auto TestPageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };

/// How long a test waits for the build thread and the extension's lane: generous, as ASan is slow.
constexpr auto ExtensionTimeout = 20'000;

[[nodiscard]] std::filesystem::path configDirectory()
{
    return { "/config" };
}

[[nodiscard]] std::filesystem::path moduleDirectory()
{
    return { "/ext" };
}

constexpr auto GitHostId = std::string_view { "git@/usr/share/contour/extensions/git.wasm" };

[[nodiscard]] std::unique_ptr<TerminalSession> makeSession(contour::ContourGuiApp& app)
{
    return contour::test::makeTestSession(
        &app.sessionsManager(), std::make_unique<vtpty::MockPty>(TestPageSize), app);
}

[[nodiscard]] contour::config::TerminalProfile& mainProfile(contour::ContourGuiApp& app)
{
    return app.config().profiles.value().at("main");
}

/// Puts the configuration beside the grants file the test reads, and a module the profiles may name.
void installFiles(TestApp& testApp)
{
    auto& fileSystem = testApp.fileSystem();
    REQUIRE(fileSystem.createDirectories(configDirectory()).has_value());
    REQUIRE(fileSystem.createDirectories(moduleDirectory()).has_value());
    auto module = std::string {};
    for (auto const byte: vtextension::testing::MinimalModule)
        module.push_back(static_cast<char>(byte));
    REQUIRE(fileSystem.writeFile(moduleDirectory() / "minimal.wasm", module).has_value());
    testApp.app().config().configFile = configDirectory() / "contour.yml";
}

[[nodiscard]] ExtensionConfig extensionAt(
    std::string name,
    std::filesystem::path path,
    contour::config::Permission runCommand = contour::config::Permission::Ask)
{
    return ExtensionConfig {
        .name = std::move(name), .path = std::move(path), .config = {}, .runCommand = runCommand
    };
}

[[nodiscard]] std::string indicatorText(TerminalSession& session)
{
    session.terminal().refreshRenderBuffer();
    return session.terminal().indicatorStatusLineDisplay().renderMainPageText();
}

/// Waits, running the GUI thread's events, until the indicator status line shows @p text.
[[nodiscard]] bool showsEventually(TerminalSession& session, std::string_view text)
{
    return QTest::qWaitFor([&] { return indicatorText(session).contains(text); }, ExtensionTimeout);
}

constexpr auto GoneNotice = std::string_view { "⚠ gone disabled: extension module not found" };

[[nodiscard]] vtextension::Manifest gitManifest()
{
    return vtextension::Manifest {
        .name = "git",
        .version = 1,
        .commands = { vtextension::DeclaredCommand {
            .name = "status",
            .argvTemplate = { "git", "-C", "{dir}", "status", "--porcelain=v2", "--branch" } } },
        .digest = "d1",
    };
}

/// What @p session's permission dialog shows now: the title, then each line, one per line; empty while
/// nothing is asked.
[[nodiscard]] std::string pendingText(TerminalSession const& session)
{
    auto const prompt = session.pendingPermission();
    if (prompt.isEmpty())
        return {};
    auto text = prompt.value(QStringLiteral("title")).toString().toStdString();
    for (auto const& line: prompt.value(QStringLiteral("lines")).toList())
        text += "\n" + line.toMap().value(QStringLiteral("text")).toString().toStdString();
    return text;
}

/// Counts into @p asks each time @p session shows an extension's question.
void countConsentAsks(TerminalSession& session, int& asks)
{
    contour::test::countAsks(
        session, contour::session::PermissionAsk { contour::session::ExtensionConsentAsk {} }, asks);
}

/// Asks @p session for permission as an extension's lane does: from a thread of its own.
void askFromAnotherThread(TerminalSession& session, vtextension::Manifest const& manifest = gitManifest())
{
    auto lane = std::thread { [&] { session.requestExtensionPermission(GitHostId, manifest); } };
    lane.join();
}
} // namespace

TEST_CASE("TerminalSession: a mirrored pane runs no extension of its own", "[contour][session][extensions]")
{
    // A pane of `contour client` (or of a tmux attach) mirrors a session whose terminal is on the remote end,
    // and so are its extensions: the profile's own would run local commands against remote state. It keeps
    // the empty forwarder its terminal was built with, and starts no runtime, thread or timer for them.
    TestApp testApp;
    installFiles(testApp);
    mainProfile(testApp.app()).extensions.value() = { extensionAt("gone", moduleDirectory() / "gone.wasm") };
    mainProfile(testApp.app()).statusLine.value().indicator.left = "<{Extension:gone}>";
    auto const session = std::make_unique<TerminalSession>(&testApp.app().sessionsManager(),
                                                           std::make_unique<vtpty::MockPty>(TestPageSize),
                                                           testApp.app(),
                                                           std::string {},
                                                           std::nullopt,
                                                           std::nullopt,
                                                           contour::test::makeTestNotifier(),
                                                           contour::session::SessionRole::Shell,
                                                           contour::session::SessionHosting::Remote);
    QCoreApplication::processEvents();

    REQUIRE(session->statusLineExtensions() != nullptr);
    CHECK(session->statusLineExtensions()->empty());
    CHECK(session->statusLineExtensions()->subscriptions().none());
    CHECK(testApp.app().startedExtensionRuntime() == nullptr);

    SECTION("a profile switch brings none either")
    {
        auto other = mainProfile(testApp.app());
        testApp.app().config().profiles.value().insert_or_assign("other", std::move(other));
        REQUIRE((*session)(contour::actions::ChangeProfile { .name = "other" }));
        CHECK(session->statusLineExtensions()->empty());
        CHECK(testApp.app().startedExtensionRuntime() == nullptr);
    }

    SECTION("while a local pane of the same profile builds them")
    {
        // The sentinel that the setup builds at all: a local pane shows the missing module's notice ...
        auto const local = makeSession(testApp.app());
        REQUIRE(showsEventually(*local, GoneNotice));
        // ... and the mirrored one, built first, still shows nothing of it.
        CHECK_FALSE(indicatorText(*session).contains(GoneNotice));
        CHECK(session->statusLineExtensions()->empty());
    }
}

TEST_CASE("TerminalSession: a profile without extensions starts nothing of them",
          "[contour][session][extensions]")
{
    // A2: an empty forwarder, subscribed to nothing, and no runtime (so no grants file read, no engine, no
    // worker, no build thread), no host and no timer.
    TestApp testApp;

    SECTION("none declared")
    {
        auto const session = makeSession(testApp.app());
        REQUIRE(session->statusLineExtensions() != nullptr);
        CHECK(session->statusLineExtensions()->empty());
        CHECK(session->statusLineExtensions()->subscriptions().none());
        CHECK(testApp.app().startedExtensionRuntime() == nullptr);
    }

    SECTION("a take-over named, but no extension declared: diagnosed, still nothing")
    {
        mainProfile(testApp.app()).statusLine.value().extension = "git";
        auto const session = makeSession(testApp.app());
        CHECK(session->statusLineExtensions()->empty());
        CHECK(testApp.app().startedExtensionRuntime() == nullptr);
    }
}

TEST_CASE("TerminalSession: a profile's extensions run in its panes", "[contour][session][extensions]")
{
    TestApp testApp;
    installFiles(testApp);
    auto& profile = mainProfile(testApp.app());

    SECTION("a module that is not there shows why, in its segment, and starts no worker")
    {
        profile.extensions.value() = { extensionAt("gone", moduleDirectory() / "gone.wasm") };
        profile.statusLine.value().indicator.left = "{Extension:gone}";
        auto const session = makeSession(testApp.app());
        auto const* const runtime = testApp.app().startedExtensionRuntime();
        REQUIRE(runtime != nullptr);
        CHECK(showsEventually(*session, GoneNotice));
        CHECK(runtime->runningWorkers() == 0);
    }

    SECTION("a module that is there runs on the workers")
    {
        profile.extensions.value() = { extensionAt("minimal", moduleDirectory() / "minimal.wasm") };
        auto const session = makeSession(testApp.app());
        auto const* const runtime = testApp.app().startedExtensionRuntime();
        REQUIRE(runtime != nullptr);
        CHECK(QTest::qWaitFor([&] { return runtime->runningWorkers() > 0; }, ExtensionTimeout));
        CHECK_FALSE(session->statusLineExtensions()->empty());
    }

    SECTION("a profile switch (as a reload) that changes them replaces them, take-over and all")
    {
        profile.extensions.value() = { extensionAt("minimal", moduleDirectory() / "minimal.wasm") };
        profile.statusLine.value().indicator.left = "template";
        auto takingOver = profile;
        takingOver.extensions.value().push_back(extensionAt("gone", moduleDirectory() / "gone.wasm"));
        takingOver.statusLine.value().extension = "gone";
        testApp.app().config().profiles.value().insert_or_assign("takeover", std::move(takingOver));
        auto const session = makeSession(testApp.app());
        REQUIRE(indicatorText(*session).contains("template"));

        REQUIRE((*session)(contour::actions::ChangeProfile { .name = "takeover" }));
        CHECK(showsEventually(*session, GoneNotice));
        CHECK_FALSE(indicatorText(*session).contains("template"));

        // And back: nothing takes over, the template is drawn again.
        REQUIRE((*session)(contour::actions::ChangeProfile { .name = "main" }));
        CHECK(showsEventually(*session, "template"));
    }

    SECTION("a reload that adds extensions to a pane built without them starts them there")
    {
        auto withExtensions = profile;
        withExtensions.extensions.value() = { extensionAt("gone", moduleDirectory() / "gone.wasm") };
        withExtensions.statusLine.value().indicator.left = "{Extension:gone}";
        testApp.app().config().profiles.value().insert_or_assign("other", std::move(withExtensions));
        auto const session = makeSession(testApp.app());
        REQUIRE(testApp.app().startedExtensionRuntime() == nullptr);

        REQUIRE((*session)(contour::actions::ChangeProfile { .name = "other" }));
        CHECK(showsEventually(*session, GoneNotice));
        CHECK(testApp.app().startedExtensionRuntime() != nullptr);

        // And a reload that drops them again stops them, at once.
        REQUIRE((*session)(contour::actions::ChangeProfile { .name = "main" }));
        CHECK(session->statusLineExtensions()->empty());
    }
}

TEST_CASE("TerminalSession: the foreground process is polled only while an extension follows it",
          "[contour][session][extensions]")
{
    // An extension that does not follow the foreground process -- a clock -- costs the GUI thread no
    // timer: the pane would otherwise wake once a second for nothing.
    TestApp testApp;
    installFiles(testApp);
    mainProfile(testApp.app()).extensions.value() = { extensionAt("minimal",
                                                                  moduleDirectory() / "minimal.wasm") };
    auto const session = makeSession(testApp.app());
    auto const* const runtime = testApp.app().startedExtensionRuntime();
    REQUIRE(runtime != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return runtime->runningWorkers() > 0; }, ExtensionTimeout));
    REQUIRE(QTest::qWaitFor([&] { return !session->statusLineExtensions()->empty(); }, ExtensionTimeout));
    // Let the load land and whatever it posts run.
    QTest::qWait(100);
    CHECK_FALSE(session->pollsForegroundProcess());
}

#ifdef CONTOUR_TEST_ECHO_WASM
TEST_CASE("TerminalSession: an extension that follows the foreground process has it polled",
          "[contour][session][extensions][guest]")
{
    TestApp testApp;
    installFiles(testApp);
    auto in = std::ifstream { CONTOUR_TEST_ECHO_WASM, std::ios::binary };
    REQUIRE(in.good());
    auto const module =
        std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    REQUIRE(testApp.fileSystem().writeFile(moduleDirectory() / "echo.wasm", module).has_value());
    auto follower = extensionAt("echo", moduleDirectory() / "echo.wasm");
    follower.config = { { "subscribe", "foreground-process" }, { "text", "following" } };
    mainProfile(testApp.app()).extensions.value() = { follower };
    mainProfile(testApp.app()).statusLine.value().indicator.left = "[{Extension:echo}]";
    auto other = mainProfile(testApp.app());
    other.extensions.value() = { extensionAt("minimal", moduleDirectory() / "minimal.wasm") };
    testApp.app().config().profiles.value().insert_or_assign("other", std::move(other));
    auto const session = makeSession(testApp.app());
    // Known only once the extension has loaded on its lane, after the build was swapped in.
    CHECK(QTest::qWaitFor([&] { return session->pollsForegroundProcess(); }, ExtensionTimeout));

    // A reload to extensions that do not follow it stops the poll.
    REQUIRE((*session)(contour::actions::ChangeProfile { .name = "other" }));
    CHECK(QTest::qWaitFor([&] { return !session->pollsForegroundProcess(); }, ExtensionTimeout));
}

TEST_CASE("TerminalSession: the foreground process is polled again after a reload",
          "[contour][session][extensions][guest]")
{
    // A reload swaps in extensions that have not loaded yet; once they have, and follow the foreground
    // process, it is polled again -- whatever the ones before them followed.
    TestApp testApp;
    installFiles(testApp);
    auto in = std::ifstream { CONTOUR_TEST_ECHO_WASM, std::ios::binary };
    REQUIRE(in.good());
    auto const module =
        std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    REQUIRE(testApp.fileSystem().writeFile(moduleDirectory() / "echo.wasm", module).has_value());
    auto followerShowing = [](std::string text) {
        auto follower = extensionAt("echo", moduleDirectory() / "echo.wasm");
        follower.config = { { "subscribe", "foreground-process" }, { "text", std::move(text) } };
        return follower;
    };
    mainProfile(testApp.app()).extensions.value() = { followerShowing("first") };
    mainProfile(testApp.app()).statusLine.value().indicator.left = "[{Extension:echo}]";
    auto second = mainProfile(testApp.app());
    second.extensions.value() = { followerShowing("second") };
    auto none = mainProfile(testApp.app());
    none.extensions.value() = {};
    testApp.app().config().profiles.value().insert_or_assign("second", std::move(second));
    testApp.app().config().profiles.value().insert_or_assign("none", std::move(none));
    auto const session = makeSession(testApp.app());
    REQUIRE(QTest::qWaitFor([&] { return session->pollsForegroundProcess(); }, ExtensionTimeout));

    SECTION("a follower replaced by another follower")
    {
        REQUIRE((*session)(contour::actions::ChangeProfile { .name = "second" }));
        // The new set has loaded and drawn: what it follows is known.
        REQUIRE(showsEventually(*session, "[second]"));
        CHECK(QTest::qWaitFor([&] { return session->pollsForegroundProcess(); }, ExtensionTimeout));
    }

    SECTION("a follower replaced by none, then by a follower")
    {
        REQUIRE((*session)(contour::actions::ChangeProfile { .name = "none" }));
        CHECK_FALSE(session->pollsForegroundProcess());
        REQUIRE((*session)(contour::actions::ChangeProfile { .name = "second" }));
        REQUIRE(showsEventually(*session, "[second]"));
        CHECK(QTest::qWaitFor([&] { return session->pollsForegroundProcess(); }, ExtensionTimeout));
    }
}
#else
TEST_CASE("TerminalSession: an extension that follows the foreground process has it polled",
          "[contour][session][extensions][guest]")
{
    SKIP("the echo test guest is not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}

TEST_CASE("TerminalSession: the foreground process is polled again after a reload",
          "[contour][session][extensions][guest]")
{
    SKIP("the echo test guest is not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif

TEST_CASE("TerminalSession: an extension asks before it runs its commands", "[contour][session][extensions]")
{
    TestApp testApp;
    installFiles(testApp);
    mainProfile(testApp.app()).extensions.value() = { extensionAt("gone", moduleDirectory() / "gone.wasm") };
    mainProfile(testApp.app()).statusLine.value().indicator.left = "{Extension:gone}";
    auto const session = makeSession(testApp.app());
    // The build thread probes the in-memory filesystem, which takes no lock: it must be done before an answer
    // writes the grants file there.
    REQUIRE(showsEventually(*session, GoneNotice));
    auto asked = 0;
    countConsentAsks(*session, asked);
    REQUIRE(pendingText(*session).empty());

    // Posted, never called on the lane: the lane does not wait for the GUI thread (A1).
    askFromAnotherThread(*session);
    CHECK(asked == 0);
    QCoreApplication::sendPostedEvents(session.get());
    REQUIRE(asked == 1);

    // The prompt says who asks, from which module, and every command verbatim.
    auto const text = pendingText(*session);
    CHECK(text.contains("“git” wants to run commands"));
    CHECK(text.contains(GitHostId));
    CHECK(text.contains("status: git -C {dir} status --porcelain=v2 --branch"));

    auto& grants = testApp.app().extensionRuntime().grants();
    auto const grantsFile = configDirectory() / "extension-grants.txt";

    SECTION("Allow is recorded for that manifest, in the grants file too")
    {
        answerShown(*session, PermissionDecision::Allow, PermissionMemory::Persistent);
        CHECK(grants.lookup(GitHostId, "d1") == GrantState::Granted);
        CHECK(pendingText(*session).empty());
        auto const written = testApp.fileSystem().readFile(grantsFile);
        REQUIRE(written.has_value());
        CHECK(written->contains(std::format("granted\td1\t{}", GitHostId)));

        // Answered: asking again shows nothing.
        askFromAnotherThread(*session);
        QCoreApplication::sendPostedEvents(session.get());
        CHECK(asked == 1);
    }

    SECTION("Deny is recorded")
    {
        answerShown(*session, PermissionDecision::Deny, PermissionMemory::Persistent);
        CHECK(grants.lookup(GitHostId, "d1") == GrantState::Denied);
    }

    SECTION("dismissing records nothing")
    {
        answerShown(*session, PermissionDecision::Deny, PermissionMemory::Once);
        CHECK(grants.lookup(GitHostId, "d1") == GrantState::Pending);
        CHECK_FALSE(testApp.fileSystem().exists(grantsFile));
        CHECK(pendingText(*session).empty());
    }

    SECTION("Allow just this once is no answer: nothing is recorded, and the question stays")
    {
        answerShown(*session, PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(grants.lookup(GitHostId, "d1") == GrantState::Pending);
        CHECK_FALSE(testApp.fileSystem().exists(grantsFile));
        CHECK(session->pendingPermissionAsk()
              == contour::session::PermissionAsk { contour::session::ExtensionConsentAsk {} });
        CHECK(pendingText(*session) == text);
    }
}

namespace
{
/// Where a daemon's answers go, as the client records them: a list.
class RecordedAnswers final: public vtextension::IGrantStore
{
  public:
    [[nodiscard]] GrantState lookup(std::string_view /*hostId*/, std::string_view /*digest*/) const override
    {
        return GrantState::Pending;
    }

    void record(std::string_view hostId, std::string_view digest, GrantState state) override
    {
        answers.push_back(std::format("{} {} {}", hostId, digest, std::to_underlying(state)));
    }

    std::vector<std::string> answers;
};
} // namespace

TEST_CASE("TerminalSession: a daemon's extension asks through the pane's prompt",
          "[contour][session][extensions]")
{
    // A daemon-hosted pane runs no extension of its own here: the daemon runs them, and asks this client's
    // user through the prompt a local pane's extensions ask through. The answer goes where the daemon's
    // question came with, never into this application's grants, and no local runtime is started for it.
    TestApp testApp;
    installFiles(testApp);
    auto const session = makeSession(testApp.app());
    auto asked = 0;
    countConsentAsks(*session, asked);
    auto daemon = RecordedAnswers {};
    constexpr auto DaemonHostId = std::string_view { "git@/usr/share/contour/extensions/git.wasm" };

    session->askForHostedExtension(
        vtextension::ExtensionConsentRequest { .hostId = std::string { DaemonHostId },
                                               .manifest = gitManifest() },
        daemon);
    REQUIRE(asked == 1);
    auto const text = pendingText(*session);
    CHECK(text.contains(DaemonHostId));
    CHECK(text.contains("status: git -C {dir} status --porcelain=v2 --branch"));

    answerShown(*session, PermissionDecision::Allow, PermissionMemory::Persistent);
    CHECK(daemon.answers
          == std::vector<std::string> {
              std::format("{} d1 {}", DaemonHostId, std::to_underlying(GrantState::Granted)) });
    CHECK(pendingText(*session).empty());
    CHECK(testApp.app().startedExtensionRuntime() == nullptr);
}

TEST_CASE("TerminalSession: an extension waits behind a guarded role", "[contour][session][extensions]")
{
    using contour::session::ExtensionConsentAsk;
    using contour::session::GuardedRole;
    using contour::session::PermissionAsk;
    TestApp testApp;
    installFiles(testApp);
    auto const session = makeSession(testApp.app());
    // Displayed: a capture without a display is refused at once, never asked.
    auto surface = contour::test::FakeDisplaySurface {};
    surface.attachedSession = session.get();
    session->attachDisplay(surface);
    auto daemon = RecordedAnswers {};

    session->requestCaptureBuffer(vtbackend::LineCount(1), vtbackend::CaptureLineMode::Physical);
    session->askForHostedExtension(vtextension::ExtensionConsentRequest { .hostId = std::string { GitHostId },
                                                                          .manifest = gitManifest() },
                                   daemon);
    CHECK(session->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });

    // The capture's answer is the capture's alone; the extension is asked next.
    auto const captureQuestion = session->pendingPermission().value(QStringLiteral("id")).toInt();
    answerShown(*session, PermissionDecision::Deny, PermissionMemory::Once);
    QCoreApplication::processEvents();
    CHECK(daemon.answers.empty());
    REQUIRE(session->pendingPermissionAsk() == PermissionAsk { ExtensionConsentAsk {} });
    CHECK(pendingText(*session).contains("“git” wants to run commands"));

    // A second answer aimed at the capture -- its old id -- grants the extension nothing.
    session->answerPermission(captureQuestion, PermissionDecision::Allow, PermissionMemory::Persistent);
    CHECK(daemon.answers.empty());
    REQUIRE(session->pendingPermissionAsk() == PermissionAsk { ExtensionConsentAsk {} });

    answerShown(*session, PermissionDecision::Deny, PermissionMemory::Persistent);
    CHECK(daemon.answers
          == std::vector<std::string> {
              std::format("{} d1 {}", GitHostId, std::to_underlying(GrantState::Denied)) });
    CHECK_FALSE(session->pendingPermissionAsk().has_value());
    session->detachDisplay(surface);
}

TEST_CASE("TerminalSession: a guest's manifest cannot turn the prompt into rich text",
          "[contour][session][extensions]")
{
    // R48: the dialog that shows the prompt guesses its format from the text up to the first line break; the
    // prompt must read as plain text whatever the guest declares, and still show what it declares verbatim.
    TestApp testApp;
    installFiles(testApp);
    mainProfile(testApp.app()).extensions.value() = { extensionAt("gone", moduleDirectory() / "gone.wasm") };
    mainProfile(testApp.app()).statusLine.value().indicator.left = "{Extension:gone}";
    auto const session = makeSession(testApp.app());
    // The build thread probes the in-memory filesystem, which takes no lock: it must be done before an answer
    // writes the grants file there.
    REQUIRE(showsEventually(*session, GoneNotice));
    auto manifest = gitManifest();
    manifest.name = "<b>git</b>";
    manifest.commands.front().argvTemplate = { "git", "<br>", "<span style=\"font-size:1px\">x</span>" };
    askFromAnotherThread(*session, manifest);
    QCoreApplication::sendPostedEvents(session.get());

    // Shown as plain text (every Label of the dialog is Text.PlainText), and verbatim.
    auto const text = pendingText(*session);
    REQUIRE_FALSE(text.empty());
    CHECK(text.contains("“<b>git</b>” wants to run commands"));
    CHECK(text.contains("git <br> '<span style=\"font-size:1px\">x</span>'"));
}

TEST_CASE("TerminalSession: a font change costs a pane without extensions nothing",
          "[contour][session][extensions]")
{
    // No font descriptions copied, no lock taken: the session asks whether it has extensions first (A2).
    TestApp testApp;
    installFiles(testApp);
    auto surface = contour::test::FakeDisplaySurface {};
    auto const session = makeSession(testApp.app());
    surface.attachedSession = session.get();
    session->attachDisplay(surface);
    session->updateStatusLineFontMetrics();
    CHECK(surface.fontDefReads == 0);
    session->detachDisplay(surface);
}

#ifdef CONTOUR_TEST_ECHO_WASM
TEST_CASE("TerminalSession: extensions a reload brings are told the font the display draws in",
          "[contour][session][extensions][guest]")
{
    // The pane had no extensions when its display was attached, so nothing was told then; the reload that
    // brings some tells them the display's current font.
    TestApp testApp;
    installFiles(testApp);
    auto in = std::ifstream { CONTOUR_TEST_ECHO_WASM, std::ios::binary };
    REQUIRE(in.good());
    auto const module =
        std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    REQUIRE(testApp.fileSystem().writeFile(moduleDirectory() / "echo.wasm", module).has_value());
    auto withEcho = mainProfile(testApp.app());
    auto follower = extensionAt("echo", moduleDirectory() / "echo.wasm");
    follower.config = { { "events", "describe" }, { "subscribe", "font" } };
    withEcho.extensions.value() = { follower };
    withEcho.statusLine.value().indicator.left = "[{Extension:echo}]";
    testApp.app().config().profiles.value().insert_or_assign("echo", std::move(withEcho));

    auto surface = contour::test::FakeDisplaySurface {};
    surface.cellSizeValue = vtbackend::ImageSize { vtbackend::Width(11), vtbackend::Height(22) };
    surface.fontDefValue.size = 14;
    auto const session = makeSession(testApp.app());
    surface.attachedSession = session.get();
    session->attachDisplay(surface);

    REQUIRE((*session)(contour::actions::ChangeProfile { .name = "echo" }));
    CHECK(showsEventually(*session, "[font-changed:14pt 11x22]"));
    session->detachDisplay(surface);
}
#else
TEST_CASE("TerminalSession: extensions a reload brings are told the font the display draws in",
          "[contour][session][extensions][guest]")
{
    SKIP("the echo test guest is not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif

#ifdef CONTOUR_TEST_ECHO_WASM
TEST_CASE("TerminalSession: the extensions are told the font, and a take-over draws at its cell size",
          "[contour][session][extensions][guest]")
{
    // The display tells the session its font when attached and on every change; the session tells the
    // terminal, the terminal its extensions. echo describes the font-changed events it follows; a take-over
    // echo asks its own surface for the cell size (CSI 16 t), and shows the answer as hex.
    TestApp testApp;
    installFiles(testApp);
    auto in = std::ifstream { CONTOUR_TEST_ECHO_WASM, std::ios::binary };
    REQUIRE(in.good());
    auto const module =
        std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    REQUIRE(testApp.fileSystem().writeFile(moduleDirectory() / "echo.wasm", module).has_value());

    auto& profile = mainProfile(testApp.app());
    auto follower = extensionAt("echo", moduleDirectory() / "echo.wasm");
    follower.config = { { "events", "describe" }, { "subscribe", "font" } };
    profile.extensions.value() = { follower };
    profile.statusLine.value().indicator.left = "[{Extension:echo}]";
    auto probing = profile;
    auto probe = extensionAt("probe", moduleDirectory() / "echo.wasm");
    probe.config = { { "text", "\033[16t" } };
    probing.extensions.value() = { probe };
    probing.statusLine.value().extension = "probe";
    testApp.app().config().profiles.value().insert_or_assign("probing", std::move(probing));

    // Declared before the session, which holds a pointer to it, so that it outlives the session.
    auto surface = contour::test::FakeDisplaySurface {};
    surface.cellSizeValue = vtbackend::ImageSize { vtbackend::Width(10), vtbackend::Height(20) };
    surface.fontDefValue.size = 12.5;
    auto const session = makeSession(testApp.app());
    surface.attachedSession = session.get();
    session->attachDisplay(surface);
    CHECK(showsEventually(*session, "[font-changed:12.5pt 10x20]"));

    // A zoom, a reload or a move to another screen: the display tells the session once it has applied it.
    surface.cellSizeValue = vtbackend::ImageSize { vtbackend::Width(12), vtbackend::Height(24) };
    surface.fontDefValue.size = 15;
    session->updateStatusLineFontMetrics();
    CHECK(showsEventually(*session, "[font-changed:15pt 12x24]"));

    // Extensions built afterwards draw at the current cell size from their first frame.
    REQUIRE((*session)(contour::actions::ChangeProfile { .name = "probing" }));
    CHECK(showsEventually(*session, "1b5b363b32343b313274")); // ESC [ 6 ; 2 4 ; 1 2 t
    session->detachDisplay(surface);
}
#else
TEST_CASE("TerminalSession: the extensions are told the font, and a take-over draws at its cell size",
          "[contour][session][extensions][guest]")
{
    SKIP("the echo test guest is not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif

#ifdef CONTOUR_TEST_SPAWNER_WASM
TEST_CASE("TerminalSession: an extension's command runs once the user allows it",
          "[contour][session][extensions][guest]")
{
    // End to end: the guest's run-command is refused as pending, its lane asks the session, the prompt is
    // queued, Allow is recorded, and the guest's next run-command (at its next render) runs the command.
    TestApp testApp;
    installFiles(testApp);
    auto in = std::ifstream { CONTOUR_TEST_SPAWNER_WASM, std::ios::binary };
    REQUIRE(in.good());
    auto const module =
        std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    REQUIRE(testApp.fileSystem().writeFile(moduleDirectory() / "spawner.wasm", module).has_value());
    auto& profile = mainProfile(testApp.app());
    profile.extensions.value() = { extensionAt("spawner", moduleDirectory() / "spawner.wasm") };
    profile.statusLine.value().indicator.left = "[{Extension:spawner}]";
    auto const session = makeSession(testApp.app());
    auto asked = 0;
    countConsentAsks(*session, asked);

    REQUIRE(QTest::qWaitFor([&] { return asked == 1; }, ExtensionTimeout));
    auto const text = pendingText(*session);
    CHECK(text.contains("“spawner”"));
    CHECK(text.contains("echo: echo {word}"));
    CHECK(showsEventually(*session, "[err:pending]"));

    answerShown(*session, PermissionDecision::Allow, PermissionMemory::Persistent);
    // A resize is what renders the guest again, and its render runs the command.
    session->terminal().resizeScreen(
        vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(90) });
    CHECK(showsEventually(*session, "[ok:0:hello"));
}
#else
TEST_CASE("TerminalSession: an extension's command runs once the user allows it",
          "[contour][session][extensions][guest]")
{
    SKIP("the spawner test guest is not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif
