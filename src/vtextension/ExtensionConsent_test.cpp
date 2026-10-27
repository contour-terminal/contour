// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/ExtensionConsent.hpp>

using vtextension::ConsentAnswer;
using vtextension::ConsentPrompt;
using vtextension::consentQuestion;
using vtextension::ConsentQuestion;
using vtextension::DeclaredCommand;
using vtextension::displayedCommandLine;
using vtextension::ExtensionConsentQueue;
using vtextension::ExtensionConsentRequest;
using vtextension::GrantState;
using vtextension::Manifest;
using vtextension::neutralizedForDisplay;

namespace
{
/// Grants in memory, keyed as the production store keys them.
class MemoryGrants final: public vtextension::IGrantStore
{
  public:
    [[nodiscard]] GrantState lookup(std::string_view hostId, std::string_view digest) const override
    {
        auto const found = _grants.find({ std::string { hostId }, std::string { digest } });
        return found != _grants.end() ? found->second : GrantState::Pending;
    }

    void record(std::string_view hostId, std::string_view digest, GrantState state) override
    {
        _grants[{ std::string { hostId }, std::string { digest } }] = state;
    }

  private:
    std::map<std::pair<std::string, std::string>, GrantState> _grants;
};

/// @return @p codePoints, UTF-8 encoded. What holds a bidirectional control is written as code points and
///         encoded here, so that no narrow literal in this file holds one (misc-misleading-bidirectional).
[[nodiscard]] std::string utf8(std::u32string_view codePoints)
{
    auto text = std::string {};
    auto const put = [&](uint32_t value) {
        text.push_back(static_cast<char>(value));
    };
    for (auto const codePoint: codePoints)
    {
        auto const value = static_cast<uint32_t>(codePoint);
        if (value < 0x80)
            put(value);
        else if (value < 0x800)
        {
            put(0xC0U | (value >> 6U));
            put(0x80U | (value & 0x3FU));
        }
        else if (value < 0x10000)
        {
            put(0xE0U | (value >> 12U));
            put(0x80U | ((value >> 6U) & 0x3FU));
            put(0x80U | (value & 0x3FU));
        }
        else
        {
            put(0xF0U | (value >> 18U));
            put(0x80U | ((value >> 12U) & 0x3FU));
            put(0x80U | ((value >> 6U) & 0x3FU));
            put(0x80U | (value & 0x3FU));
        }
    }
    return text;
}

[[nodiscard]] ExtensionConsentRequest gitRequest(std::string digest = "d1")
{
    return ExtensionConsentRequest {
        .hostId = "git@/usr/share/contour/extensions/git.wasm",
        .manifest =
            Manifest {
                .name = "git",
                .version = 1,
                .commands = { DeclaredCommand {
                    .name = "status",
                    .argvTemplate = { "git", "-C", "{dir}", "status", "--porcelain=v2", "--branch" } } },
                .digest = std::move(digest) },
    };
}
} // namespace

TEST_CASE("ExtensionConsent: text a guest chose is shown as what it is", "[vtextension][extensions]")
{
    struct Case
    {
        std::string_view name;
        std::string input;
        std::string_view expected;
    };
    auto const cases = std::vector<Case> {
        { .name = "plain text", .input = "git status --branch", .expected = "git status --branch" },
        { .name = "other scripts are kept", .input = utf8(U"größe ✓ 日本"), .expected = "größe ✓ 日本" },
        { .name = "C0 controls", .input = "a\nb\tc\x1b[2J", .expected = "a<U+000A>b<U+0009>c<U+001B>[2J" },
        { .name = "DEL and C1", .input = utf8(U"a\x7f\x9b"), .expected = "a<U+007F><U+009B>" },
        { .name = "right-to-left override",
          .input = utf8(U"safe\u202Eexe.sh"),
          .expected = "safe<U+202E>exe.sh" },
        { .name = "every bidi embedding and override",
          .input = utf8(U"\u202A\u202B\u202C\u202D"),
          .expected = "<U+202A><U+202B><U+202C><U+202D>" },
        { .name = "bidi isolates",
          .input = utf8(U"\u2066\u2067\u2068\u2069"),
          .expected = "<U+2066><U+2067><U+2068><U+2069>" },
        { .name = "bidi marks",
          .input = utf8(U"\u200E\u200F\u061C"),
          .expected = "<U+200E><U+200F><U+061C>" },
        { .name = "zero-width characters",
          .input = utf8(U"r\u200Bm \u200C\u200D\u2060\uFEFF"),
          .expected = "r<U+200B>m <U+200C><U+200D><U+2060><U+FEFF>" },
        { .name = "line and paragraph separators",
          .input = utf8(U"a\u2028b\u2029c"),
          .expected = "a<U+2028>b<U+2029>c" },
        { .name = "soft hyphen", .input = utf8(U"rm\u00AD-rf"), .expected = "rm<U+00AD>-rf" },
        { .name = "tag characters", .input = utf8(U"x\U000E0041"), .expected = "x<U+E0041>" },
        { .name = "bytes that are not UTF-8", .input = "a\xff\xc3", .expected = "a<0xFF><0xC3>" },
    };
    for (auto const& testCase: cases)
    {
        INFO(testCase.name);
        CHECK(neutralizedForDisplay(testCase.input) == testCase.expected);
    }
}

TEST_CASE("ExtensionConsent: the command line is the full argv template", "[vtextension][extensions]")
{
    CHECK(displayedCommandLine(gitRequest().manifest.commands.front())
          == "git -C {dir} status --porcelain=v2 --branch");
    CHECK(displayedCommandLine(DeclaredCommand { .name = "x", .argvTemplate = { "echo", "a b", "", "c" } })
          == "echo 'a b' '' c");
    CHECK(displayedCommandLine(DeclaredCommand { .name = "x", .argvTemplate = { utf8(U"rm\u202E"), "-rf" } })
          == "rm<U+202E> -rf");
}

namespace
{
/// The texts of @p question's lines, in order.
[[nodiscard]] std::vector<std::string> textsOf(ConsentQuestion const& question)
{
    auto texts = std::vector<std::string> {};
    for (auto const& line: question.lines)
        texts.push_back(line.text);
    return texts;
}

/// The one line of @p question whose text is @p text; fails the test without one.
[[nodiscard]] vtextension::ConsentLine const& lineOf(ConsentQuestion const& question, std::string_view text)
{
    auto const found = std::ranges::find(question.lines, text, &vtextension::ConsentLine::text);
    REQUIRE(found != question.lines.end());
    return *found;
}
} // namespace

TEST_CASE("ExtensionConsent: the prompt says who asks, from where, for what", "[vtextension][consent]")
{
    auto request = gitRequest();
    request.manifest.name = utf8(U"g‮it");
    auto const question = consentQuestion(request);
    CHECK(question.title == "“g<U+202E>it” wants to run commands");
    CHECK_FALSE(question.title.contains(utf8(U"‮")));
    std::ignore = lineOf(question, "Module: git@/usr/share/contour/extensions/git.wasm");
    std::ignore = lineOf(question, "status: git -C {dir} status --porcelain=v2 --branch");
}

TEST_CASE("ExtensionConsent: what a guest chose is shown as text", "[vtextension][consent]")
{
    auto request = gitRequest();
    request.manifest.name = "<b>git</b>";
    request.manifest.commands.front().argvTemplate = { "git",
                                                       "<br>",
                                                       "<span style=\"font-size:1px\">x</span>" };
    auto const question = consentQuestion(request);
    CHECK(question.title == "“<b>git</b>” wants to run commands");
    std::ignore = lineOf(question, "status: git <br> '<span style=\"font-size:1px\">x</span>'");
}

TEST_CASE("ExtensionConsent: a prompt for commands only", "[vtextension][consent]")
{
    auto const question = consentQuestion(gitRequest());
    CHECK(question.title == "“git” wants to run commands");
    CHECK(textsOf(question)
          == std::vector<std::string> {
              "Module: git@/usr/share/contour/extensions/git.wasm",
              "It may run only these, exactly as written ({name} is filled in by the extension):",
              "status: git -C {dir} status --porcelain=v2 --branch",
          });
    for (auto const& line: question.lines)
        CHECK(line.weight == vtcontrol::ConsentWeight::Observes);
}

TEST_CASE("consent lists control permissions", "[vtextension][consent][control]")
{
    using vtcontrol::ConsentWeight;
    using vtcontrol::Permission;
    auto request = gitRequest();
    request.manifest.name = "git-stats";
    request.manifest.control = { Permission::Read, Permission::Input };
    auto const question = consentQuestion(request);
    CHECK(question.title == "“git-stats” wants to control Contour");
    CHECK(textsOf(question)
          == std::vector<std::string> {
              "Module: git@/usr/share/contour/extensions/git.wasm",
              "It may run only these, exactly as written ({name} is filled in by the extension):",
              "status: git -C {dir} status --porcelain=v2 --branch",
              "May see the windows, tabs and panes.",
              "May type into panes (can run commands).",
          });
    CHECK(lineOf(question, "May type into panes (can run commands).").weight
          == ConsentWeight::CanRunCommands);
    CHECK(lineOf(question, "May see the windows, tabs and panes.").weight == ConsentWeight::Observes);

    SECTION("without commands")
    {
        request.manifest.commands.clear();
        request.manifest.control = { Permission::Screen, Permission::Write };
        auto const controlOnly = consentQuestion(request);
        CHECK(controlOnly.title == "“git-stats” wants to control Contour");
        CHECK(textsOf(controlOnly)
              == std::vector<std::string> {
                  "Module: git@/usr/share/contour/extensions/git.wasm",
                  "May read what panes show, and their command history.",
                  "May open, split, close and label tabs and panes (can run commands).",
              });
    }
}

TEST_CASE("ExtensionConsentQueue: one prompt at a time, each answer recorded", "[vtextension][extensions]")
{
    auto grants = MemoryGrants {};
    auto queue = ExtensionConsentQueue {};
    REQUIRE(queue.current() == nullptr);

    SECTION("the first question is due; Allow records Granted for its digest")
    {
        CHECK(queue.offer(gitRequest(), grants) == ConsentPrompt::Due);
        REQUIRE(queue.current() != nullptr);
        CHECK(queue.current()->hostId == gitRequest().hostId);
        CHECK(queue.answer(ConsentAnswer::Allowed) == ConsentPrompt::None);
        CHECK(grants.lookup(gitRequest().hostId, "d1") == GrantState::Granted);
        CHECK(queue.current() == nullptr);
    }

    SECTION("Deny records Denied; Dismiss records nothing")
    {
        CHECK(queue.offer(gitRequest("d1"), grants) == ConsentPrompt::Due);
        CHECK(queue.answer(ConsentAnswer::Denied) == ConsentPrompt::None);
        CHECK(grants.lookup(gitRequest().hostId, "d1") == GrantState::Denied);
        CHECK(queue.offer(gitRequest("d2"), grants) == ConsentPrompt::Due);
        CHECK(queue.answer(ConsentAnswer::Dismissed) == ConsentPrompt::None);
        CHECK(grants.lookup(gitRequest().hostId, "d2") == GrantState::Pending);
    }

    SECTION("a second question waits for the first, and is due once that is answered")
    {
        CHECK(queue.offer(gitRequest("d1"), grants) == ConsentPrompt::Due);
        CHECK(queue.offer(gitRequest("d2"), grants) == ConsentPrompt::None);
        CHECK(queue.current()->manifest.digest == "d1");
        CHECK(queue.answer(ConsentAnswer::Allowed) == ConsentPrompt::Due);
        CHECK(queue.current()->manifest.digest == "d2");
    }

    SECTION("a question already waiting, or already answered, is not asked again")
    {
        CHECK(queue.offer(gitRequest("d1"), grants) == ConsentPrompt::Due);
        CHECK(queue.offer(gitRequest("d1"), grants) == ConsentPrompt::None);
        CHECK(queue.answer(ConsentAnswer::Allowed) == ConsentPrompt::None);
        CHECK(queue.offer(gitRequest("d1"), grants) == ConsentPrompt::None);
        CHECK(queue.current() == nullptr);
    }

    SECTION("a waiting question answered meanwhile elsewhere is skipped")
    {
        CHECK(queue.offer(gitRequest("d1"), grants) == ConsentPrompt::Due);
        CHECK(queue.offer(gitRequest("d2"), grants) == ConsentPrompt::None);
        grants.record(gitRequest().hostId, "d2", GrantState::Denied);
        CHECK(queue.answer(ConsentAnswer::Allowed) == ConsentPrompt::None);
        CHECK(queue.current() == nullptr);
    }

    SECTION("an answer without a question does nothing")
    {
        CHECK(queue.answer(ConsentAnswer::Allowed) == ConsentPrompt::None);
    }

    SECTION("each answer is recorded where its question was asked for")
    {
        // A daemon-hosted pane asks for the daemon's extensions too, whose answers the daemon keeps: the same
        // installation and manifest are two questions there.
        auto daemon = MemoryGrants {};
        CHECK(queue.offer(gitRequest("d1"), grants) == ConsentPrompt::Due);
        CHECK(queue.offer(gitRequest("d1"), daemon) == ConsentPrompt::None);
        CHECK(queue.answer(ConsentAnswer::Allowed) == ConsentPrompt::Due);
        CHECK(grants.lookup(gitRequest().hostId, "d1") == GrantState::Granted);
        CHECK(daemon.lookup(gitRequest().hostId, "d1") == GrantState::Pending);
        CHECK(queue.answer(ConsentAnswer::Denied) == ConsentPrompt::None);
        CHECK(daemon.lookup(gitRequest().hostId, "d1") == GrantState::Denied);
        CHECK(grants.lookup(gitRequest().hostId, "d1") == GrantState::Granted);
    }
}

TEST_CASE("ExtensionConsent: a guest's log line reads as what it is", "[vtextension][extensions]")
{
    // The message is the guest's own text: a log shown in a terminal must not carry its escape sequences.
    CHECK(vtextension::extensionLogLine("git", vtextension::LogLevel::Warn, "dirty\033[2J")
          == "git [warning]: dirty<U+001B>[2J");
    CHECK(vtextension::extensionLogLine("git", vtextension::LogLevel::Trace, "x") == "git [trace]: x");
    CHECK(vtextension::extensionLogLine("git", static_cast<vtextension::LogLevel>(99), "x") == "git [?]: x");
}
