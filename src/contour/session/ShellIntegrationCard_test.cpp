// SPDX-License-Identifier: Apache-2.0
//
// The shell-integration card as a user meets it: a session whose Return presses are counted, the manager
// that decides whether to offer, and the press that installs. Every test runs over a RecordingShellRcEditor
// and an in-memory GUI state, so none can append to a real rc file.

#include <contour/config/ShellKind.hpp>
#include <contour/platform/ShellRcEditor.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <vtpty/MockPty.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using contour::config::ShellKind;
using contour::platform::RcInspection;
using contour::platform::ShellRcError;
using contour::session::TerminalSession;
using contour::test::MockPtySessionFactory;
using contour::test::ScopedController;
using contour::test::TestApp;
using contour::window::CardStage;
using contour::window::ShellIntegrationProbeCommands;
using vtbackend::Key;
using vtbackend::KeyboardEventType;
using vtbackend::Modifier;
using vtbackend::Modifiers;

namespace
{

constexpr auto OfferStage = static_cast<int>(CardStage::Offer);
constexpr auto ConfirmStage = static_cast<int>(CardStage::Confirm);
constexpr auto HiddenStage = static_cast<int>(CardStage::Hidden);

/// An app whose windows hold mock PTYs, a window, and the session of its first tab, with zsh in front.
struct Rig
{
    MockPtySessionFactory* factory = nullptr;
    TestApp app;
    ScopedController window;
    TerminalSession* session = nullptr;

    explicit Rig(std::string attachedHostLabel = {}):
        app { [this] {
                 auto made = std::make_unique<MockPtySessionFactory>();
                 factory = made.get();
                 return made;
             }(),
              nullptr,
              nullptr,
              nullptr,
              TestApp::testHostIdentity(),
              nullptr,
              {},
              nullptr,
              contour::platform::nullAppearanceServices(),
              nullptr,
              std::move(attachedHostLabel) },
        window { app.manager() }
    {
        scriptZshRc(contour::config::RcState::Absent);
        session = openTab();
    }

    /// Opens a tab, and names @p foreground as the program in front of its shell.
    TerminalSession* openTab(std::string foreground = "zsh")
    {
        window->createNewTab();
        auto* const modelWindow = app.manager().model().window(window.id);
        REQUIRE(modelWindow != nullptr);
        auto* const tab = modelWindow->tabAt(window->activeTabIndex());
        REQUIRE(tab != nullptr);
        auto* const created = app.manager().sessionForId(tab->activePane()->session());
        REQUIRE(created != nullptr);
        contour::test::mockPtyOf(*created).setForegroundProcessName(std::move(foreground));
        return created;
    }

    /// The startup file zsh would use, under the home the manager shortens paths by.
    [[nodiscard]] std::filesystem::path zshRc()
    {
        return std::filesystem::path { app.manager().homeDirectory() } / ".zshrc";
    }

    void scriptZshRc(contour::config::RcState state)
    {
        app.shellRcEditor().inspections[ShellKind::Zsh] = RcInspection { .file = zshRc(), .state = state };
    }

    static void pressReturn(TerminalSession& target, int times = 1)
    {
        std::ranges::for_each(std::views::iota(0, times), [&](int) {
            target.sendKeyEvent(
                Key::Enter, Modifiers {}, KeyboardEventType::Press, std::chrono::steady_clock::now());
        });
    }

    void pressReturn(int times = 1) const { pressReturn(*session, times); }
};

} // namespace

TEST_CASE("shell integration card: three commands without a prompt mark offer it",
          "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.pressReturn(ShellIntegrationProbeCommands - 1);
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);

    rig.pressReturn();
    CHECK(rig.session->shellIntegrationCard() == OfferStage);
    CHECK(rig.session->shellIntegrationTitle() == QStringLiteral("Turn on command blocks"));
    CHECK(rig.session->shellIntegrationBody().contains(QStringLiteral("zsh")));
    CHECK(rig.session->shellIntegrationPrimary() == QStringLiteral("Install"));
    CHECK(rig.session->shellIntegrationSecondary() == QStringLiteral("Not Now"));
    CHECK(rig.app.shellRcEditor().installs.empty());
}

TEST_CASE("shell integration card: a prompt mark ends the probe", "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.pressReturn(ShellIntegrationProbeCommands - 1);
    rig.session->terminal().writeToScreen("\033]133;A\033\\$ ");
    rig.pressReturn(5);
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
}

TEST_CASE("shell integration card: Returns in an alternate-screen program do not count",
          "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.session->terminal().writeToScreen("\033[?1049h");
    rig.pressReturn(ShellIntegrationProbeCommands);
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
}

TEST_CASE("shell integration card: only a bare Return press counts", "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    auto const now = std::chrono::steady_clock::now();
    std::ranges::for_each(std::views::iota(0, ShellIntegrationProbeCommands + 2), [&](int) {
        rig.session->sendKeyEvent(Key::Enter, Modifiers {}, KeyboardEventType::Release, now);
        rig.session->sendKeyEvent(Key::Enter, Modifiers { Modifier::Shift }, KeyboardEventType::Press, now);
        rig.session->sendKeyEvent(Key::Escape, Modifiers {}, KeyboardEventType::Press, now);
    });
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
}

TEST_CASE("shell integration card: ssh, tmux and an unknown program in front offer nothing",
          "[contour][session][shellintegration]")
{
    for (auto const* const program: { "ssh", "tmux", "sudo", "zsh5", "" })
    {
        INFO(program);
        auto rig = Rig {};
        auto* const other = rig.openTab(program);
        Rig::pressReturn(*other, ShellIntegrationProbeCommands);
        CHECK(other->shellIntegrationCard() == HiddenStage);
    }
}

TEST_CASE("shell integration card: a remote-hosted window offers nothing",
          "[contour][session][shellintegration]")
{
    auto rig = Rig { "starfive" };
    rig.pressReturn(ShellIntegrationProbeCommands);
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
}

TEST_CASE("shell integration card: a shell whose rc file loads the script, or cannot be read, offers nothing",
          "[contour][session][shellintegration]")
{
    auto installed = Rig {};
    installed.scriptZshRc(contour::config::RcState::Installed);
    installed.pressReturn(ShellIntegrationProbeCommands);
    CHECK(installed.session->shellIntegrationCard() == HiddenStage);

    auto unknown = Rig {};
    unknown.scriptZshRc(contour::config::RcState::Unknown);
    unknown.pressReturn(ShellIntegrationProbeCommands);
    CHECK(unknown.session->shellIntegrationCard() == HiddenStage);

    auto unreadable = Rig {};
    unreadable.app.shellRcEditor().inspections[ShellKind::Zsh] = std::unexpected(ShellRcError::Unreadable);
    unreadable.pressReturn(ShellIntegrationProbeCommands);
    CHECK(unreadable.session->shellIntegrationCard() == HiddenStage);
}

TEST_CASE("shell integration card: Install shows the line first and writes only on Add Line",
          "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.pressReturn(ShellIntegrationProbeCommands);
    REQUIRE(rig.session->shellIntegrationCard() == OfferStage);

    rig.session->acceptShellIntegrationCard();
    CHECK(rig.session->shellIntegrationCard() == ConfirmStage);
    auto const expectedLine = contour::window::shellRcLine(ShellKind::Zsh, TestApp::ContourBinary).value();
    CHECK(rig.session->shellIntegrationBody().toStdString() == expectedLine);
    CHECK(rig.session->shellIntegrationTitle() == QStringLiteral("Add this line to ~/.zshrc?"));
    CHECK(rig.session->shellIntegrationPrimary() == QStringLiteral("Add Line"));
    CHECK(rig.session->shellIntegrationSecondary() == QStringLiteral("Cancel"));
    CHECK(rig.app.shellRcEditor().installs.empty());

    rig.session->acceptShellIntegrationCard();
    REQUIRE(rig.app.shellRcEditor().installs.size() == 1);
    CHECK(
        rig.app.shellRcEditor().installs.front()
        == contour::test::RecordingShellRcEditor::Install { .shell = ShellKind::Zsh, .line = expectedLine });
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
    CHECK(rig.window->notices().toastText()
          == QStringLiteral("Added shell integration to ~/.zshrc. New zsh sessions show command blocks."));
}

TEST_CASE("shell integration card: a failed write keeps the card and says why",
          "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.app.shellRcEditor().installResult = std::unexpected(ShellRcError::Unwritable);
    rig.pressReturn(ShellIntegrationProbeCommands);
    rig.session->acceptShellIntegrationCard();
    rig.session->acceptShellIntegrationCard();

    CHECK(rig.session->shellIntegrationCard() == ConfirmStage);
    CHECK(rig.session->shellIntegrationError()
          == QStringLiteral("~/.zshrc was not changed: the file could not be written."));
    CHECK(rig.window->notices().toastText()
          == QStringLiteral("~/.zshrc was not changed: the file could not be written."));
    CHECK(rig.window->notices().toastGlyph() == QStringLiteral("!"));
}

TEST_CASE("shell integration card: Not Now is remembered for the shell",
          "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.pressReturn(ShellIntegrationProbeCommands);
    rig.session->declineShellIntegrationCard();
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
    CHECK(rig.app.guiState().state.dismissedShellIntegrations == std::vector { ShellKind::Zsh });
    CHECK(rig.app.shellRcEditor().installs.empty());
}

TEST_CASE("shell integration card: a shell that was declined is not offered again",
          "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.app.guiState().state.dismissedShellIntegrations = { ShellKind::Zsh };
    rig.pressReturn(ShellIntegrationProbeCommands);
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);

    auto other = Rig {};
    other.app.guiState().state.dismissedShellIntegrations = { ShellKind::Fish, ShellKind::Bash };
    other.pressReturn(ShellIntegrationProbeCommands);
    CHECK(other.session->shellIntegrationCard() == OfferStage);
}

TEST_CASE("shell integration card: Cancel on the line hides the card for the run and remembers nothing",
          "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.pressReturn(ShellIntegrationProbeCommands);
    rig.session->acceptShellIntegrationCard();
    rig.session->declineShellIntegrationCard();
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
    CHECK(rig.app.guiState().state.dismissedShellIntegrations.empty());
    CHECK(rig.app.shellRcEditor().installs.empty());

    // At most one card per run: another session crossing the threshold is not offered one.
    auto* const next = rig.openTab();
    Rig::pressReturn(*next, ShellIntegrationProbeCommands);
    CHECK(next->shellIntegrationCard() == HiddenStage);
}

TEST_CASE("shell integration card: one card per run, in the first session to cross the threshold",
          "[contour][session][manager][shellintegration]")
{
    auto rig = Rig {};
    auto* const second = rig.openTab();
    Rig::pressReturn(*second, ShellIntegrationProbeCommands);
    CHECK(second->shellIntegrationCard() == OfferStage);

    rig.pressReturn(ShellIntegrationProbeCommands);
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
}

TEST_CASE("shell integration card: an ended pane hides the card", "[contour][session][shellintegration]")
{
    auto rig = Rig {};
    rig.pressReturn(ShellIntegrationProbeCommands);
    REQUIRE(rig.session->shellIntegrationCard() == OfferStage);

    contour::test::mockPtyOf(*rig.session).close();
    rig.session->onClosed();
    CHECK(rig.session->getSessionEnd() == static_cast<int>(contour::session::EndReason::Exited));
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);
}

TEST_CASE("shell integration card: an ended pane takes no card and no slot",
          "[contour][session][manager][shellintegration]")
{
    auto rig = Rig {};
    contour::test::mockPtyOf(*rig.session).close();
    rig.session->onClosed();
    rig.app.manager().offerShellIntegration(*rig.session, ShellKind::Zsh);
    CHECK(rig.session->shellIntegrationCard() == HiddenStage);

    auto* const live = rig.openTab();
    Rig::pressReturn(*live, ShellIntegrationProbeCommands);
    CHECK(live->shellIntegrationCard() == OfferStage);
}

TEST_CASE("shell integration card: TestApp never reaches a file editor",
          "[contour][session][manager][shellintegration]")
{
    TestApp app;
    CHECK(dynamic_cast<contour::test::RecordingShellRcEditor*>(&app.app().shellRcEditor()) != nullptr);
    CHECK(app.app().contourBinaryPath() == std::filesystem::path { TestApp::ContourBinary });
}
