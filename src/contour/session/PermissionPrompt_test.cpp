// SPDX-License-Identifier: Apache-2.0
#include <contour/session/PastePreview.hpp>
#include <contour/session/PermissionPrompt.hpp>
#include <contour/session/PermissionQueue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <utility>

using contour::session::GuardedRole;
using contour::session::GuardedRoleRows;
using contour::session::PermissionDecision;
using contour::session::PermissionMemory;
using contour::session::PermissionPrompt;

namespace
{

/// The title and buttons a prompt shows, and the button Return takes.
struct PromptFace
{
    std::string title;
    std::string denyText;
    std::string allowText;
    PermissionDecision defaultDecision;

    bool operator==(PromptFace const&) const = default;
};

[[nodiscard]] PromptFace faceOf(PermissionPrompt const& prompt)
{
    return { .title = prompt.title,
             .denyText = prompt.denyText,
             .allowText = prompt.allowText,
             .defaultDecision = prompt.defaultDecision };
}

/// Every guarded prompt offers the session-long checkbox, unticked.
void checkRemembersForTheSession(PermissionPrompt const& prompt)
{
    CHECK(prompt.rememberText == contour::session::SessionRememberText);
    CHECK(prompt.rememberScope == PermissionMemory::Session);
    CHECK(prompt.initialMemory == PermissionMemory::Once);
    CHECK(prompt.allowOnce == contour::session::AllowOnce::Offered);
}

} // namespace

TEST_CASE("PermissionPrompt: the rows cover every guarded role, in enumerator order",
          "[contour][permission][prompt]")
{
    for (auto const index: std::views::iota(std::size_t { 0 }, GuardedRoleRows.size()))
        CHECK(std::to_underlying(GuardedRoleRows[index].role) == index);
    CHECK(GuardedRoleRows.back().role == GuardedRole::RerunCommand);
}

TEST_CASE("PermissionPrompt: configuredPermission reads each role's own setting",
          "[contour][permission][prompt]")
{
    using contour::config::Permission;
    for (auto const& row: GuardedRoleRows)
    {
        INFO(std::format("{}", row.role));
        auto permissions = contour::config::PermissionsConfig {
            .captureBuffer = Permission::Deny,
            .changeFont = Permission::Deny,
            .displayHostWritableStatusLine = Permission::Deny,
        };
        if (row.setting == nullptr)
        {
            CHECK(contour::session::configuredPermission(permissions, row.role) == Permission::Ask);
            continue;
        }
        permissions.*row.setting = Permission::Allow;
        CHECK(contour::session::configuredPermission(permissions, row.role) == Permission::Allow);
        // The other roles still read their own, unchanged, setting.
        for (auto const& other: GuardedRoleRows)
            if (other.role != row.role && other.setting != nullptr)
                CHECK(contour::session::configuredPermission(permissions, other.role) == Permission::Deny);
    }
    auto const defaults = contour::config::PermissionsConfig {};
    CHECK(contour::session::configuredPermission(defaults, GuardedRole::BigPaste) == Permission::Ask);
    CHECK(contour::session::configuredPermission(defaults, GuardedRole::RerunCommand) == Permission::Ask);
}

TEST_CASE("PermissionPrompt: each prompt names its action", "[contour][permission][prompt]")
{
    using namespace contour::session;

    auto font = vtbackend::FontDef {};
    font.size = 14.0;
    font.regular = "Iosevka";
    auto const fontPrompt = fontChangePrompt("zsh", font);
    CHECK(
        faceOf(fontPrompt)
        == PromptFace { "Allow “zsh” to change the font?", "Deny", "Change Font", PermissionDecision::Deny });
    REQUIRE(fontPrompt.lines.size() == 1);
    CHECK(fontPrompt.lines.front().text == "Iosevka, 14 pt");
    CHECK(fontChangePrompt("zsh", vtbackend::FontDef {}).lines.front().text == "It asks for another font.");
    checkRemembersForTheSession(fontPrompt);

    auto const capturePrompt = bufferCapturePrompt("zsh", vtbackend::LineCount(24));
    CHECK(faceOf(capturePrompt)
          == PromptFace { "Allow “zsh” to read the screen?", "Deny", "Allow", PermissionDecision::Deny });
    REQUIRE(capturePrompt.lines.size() == 1);
    CHECK(capturePrompt.lines.front().text
          == "It asks for the last 24 lines of the terminal, to read them back.");
    checkRemembersForTheSession(capturePrompt);

    auto const statusLinePrompt = hostStatusLinePrompt("zsh");
    CHECK(faceOf(statusLinePrompt)
          == PromptFace {
              "Allow “zsh” to show its own status line?", "Deny", "Show", PermissionDecision::Deny });
    REQUIRE(statusLinePrompt.lines.size() == 1);
    CHECK(statusLinePrompt.lines.front().text == "It writes the text of the line below the terminal.");
    checkRemembersForTheSession(statusLinePrompt);

    auto const preview = PastePreview { .lines = { "line 0", "line 1" }, .moreLines = 48'211 };
    auto const pastePrompt = bigPastePrompt("zsh", 2'400'000, preview);
    CHECK(faceOf(pastePrompt)
          == PromptFace { "Paste 2.4 MB into “zsh”?", "Cancel", "Paste", PermissionDecision::Allow });
    REQUIRE(pastePrompt.lines.size() == preview.lines.size() + 1);
    CHECK(pastePrompt.lines[0].text == "line 0");
    CHECK(pastePrompt.lines[0].style == PromptLineStyle::Verbatim);
    CHECK(pastePrompt.lines.back().text == "… 48211 more lines");
    CHECK(pastePrompt.lines.back().style == PromptLineStyle::Prose);
    CHECK(bigPastePrompt("zsh", 2'400'000, PastePreview { .lines = { "a" }, .moreLines = 0 }).lines.size()
          == 1);
    checkRemembersForTheSession(pastePrompt);

    auto const rerun = rerunPrompt("make -j8");
    CHECK(faceOf(rerun)
          == PromptFace { "Run this command again?", "Cancel", "Run", PermissionDecision::Deny });
    REQUIRE(rerun.lines.size() == 2);
    CHECK(rerun.lines[0].text == "make -j8");
    CHECK(rerun.lines[0].style == PromptLineStyle::Verbatim);
    CHECK(rerun.lines[1].text == "It is typed at the prompt and entered.");
    checkRemembersForTheSession(rerun);
}

TEST_CASE("PermissionPrompt: a program-chosen title is shown as text", "[contour][permission][prompt]")
{
    using namespace contour::session;

    auto const prompt = bufferCapturePrompt("<img src=x>\x1b]0;evil\x07", vtbackend::LineCount(1));
    CHECK(prompt.title.contains("<img src=x>"));
    CHECK_FALSE(prompt.title.contains('\x1b'));
    CHECK_FALSE(prompt.title.contains('\x07'));

    CHECK(bufferCapturePrompt("", vtbackend::LineCount(1)).title
          == "Allow this terminal to read the screen?");

    // A long title is cut, at a character, and says so.
    auto const longTitle = std::string(200, 'z');
    auto const cut = hostStatusLinePrompt(longTitle).title;
    CHECK(cut.contains("…”"));
    CHECK(cut.size() < longTitle.size());
}

TEST_CASE("PermissionQueue: one question at a time, in the order asked", "[contour][permission][prompt]")
{
    using namespace contour::session;

    auto const a = PermissionAsk { GuardedRole::ChangeFont };
    auto const b = PermissionAsk { GuardedRole::CaptureBuffer };
    auto queue = PermissionQueue {};

    CHECK(queue.offer(a) == PromptChange::Due);
    CHECK(queue.offer(b) == PromptChange::None);
    CHECK(queue.offer(a) == PromptChange::None);
    CHECK(queue.size() == 2);
    CHECK(queue.current() == a);

    CHECK(queue.finish() == PromptChange::Due);
    CHECK(queue.current() == b);
    CHECK(queue.finish() == PromptChange::None);
    CHECK_FALSE(queue.current().has_value());
    CHECK(queue.finish() == PromptChange::None);

    // A question settled without its answer leaves the line, wherever it waits.
    CHECK(queue.offer(a) == PromptChange::Due);
    CHECK(queue.offer(b) == PromptChange::None);
    CHECK(queue.contains(b));
    CHECK(queue.withdraw(b) == PromptChange::None);
    CHECK_FALSE(queue.contains(b));
    CHECK(queue.current() == a);
    CHECK(queue.offer(b) == PromptChange::None);
    CHECK(queue.withdraw(a) == PromptChange::Due);
    CHECK(queue.current() == b);
    CHECK(queue.withdraw(b) == PromptChange::None);
    CHECK(queue.withdraw(b) == PromptChange::None);
    CHECK(queue.size() == 0);

    // An extension's question waits in the same line.
    CHECK(queue.offer(PermissionAsk { ExtensionConsentAsk {} }) == PromptChange::Due);
    CHECK(queue.offer(a) == PromptChange::None);
    CHECK(queue.current() == PermissionAsk { ExtensionConsentAsk {} });
}

TEST_CASE("PermissionPrompt: an answer to an extension's consent", "[contour][permission][prompt]")
{
    using contour::session::ConsentAnswerError;
    using contour::session::ConsentVerdict;
    using contour::session::consentVerdictFor;
    CHECK(consentVerdictFor(PermissionDecision::Allow, PermissionMemory::Persistent)
          == ConsentVerdict::Allow);
    CHECK(consentVerdictFor(PermissionDecision::Deny, PermissionMemory::Persistent) == ConsentVerdict::Deny);
    CHECK(consentVerdictFor(PermissionDecision::Deny, PermissionMemory::Once) == ConsentVerdict::Dismiss);
    CHECK(consentVerdictFor(PermissionDecision::Allow, PermissionMemory::Once)
          == std::unexpected { ConsentAnswerError::AllowMustBeRemembered });
    CHECK(consentVerdictFor(PermissionDecision::Allow, PermissionMemory::Session)
          == std::unexpected { ConsentAnswerError::ScopeNotOffered });
    CHECK(consentVerdictFor(PermissionDecision::Deny, PermissionMemory::Session)
          == std::unexpected { ConsentAnswerError::ScopeNotOffered });
}

TEST_CASE("PermissionPrompt: an extension's consent is remembered, and what can run commands stands out",
          "[contour][permission][prompt]")
{
    using namespace contour::session;
    auto const prompt = consentPrompt(
        "“git-stats” wants to control Contour",
        { consentLine("May see the windows, tabs and panes.", vtcontrol::ConsentWeight::Observes),
          consentLine("May type into panes (can run commands).", vtcontrol::ConsentWeight::CanRunCommands) });
    CHECK(prompt.title == "“git-stats” wants to control Contour");
    CHECK(prompt.denyText == "Deny");
    CHECK(prompt.allowText == "Allow");
    CHECK(prompt.rememberText == ExtensionRememberText);
    CHECK(prompt.rememberScope == PermissionMemory::Persistent);
    CHECK(prompt.initialMemory == PermissionMemory::Persistent);
    CHECK(prompt.defaultDecision == PermissionDecision::Deny);
    CHECK(prompt.allowOnce == AllowOnce::Withheld);
    REQUIRE(prompt.lines.size() == 2);
    CHECK(prompt.lines[0].weight == PromptLineWeight::Regular);
    CHECK(prompt.lines[1].weight == PromptLineWeight::Strong);
}
