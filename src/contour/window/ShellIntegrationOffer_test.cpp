// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ShellIntegration.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

using namespace contour::window;
using namespace contour::config;
using vtbackend::ScreenType;

namespace
{
constexpr auto Absent = PromptMarks::Absent;
constexpr auto Primary = ScreenType::Primary;
constexpr auto Alternate = ScreenType::Alternate;

constexpr OfferInputs AllYes {
    .shell = ShellKind::Zsh,
    .locality = RcLocality::ThisMachine,
    .host = HostPlatform::MacOS,
    .dismissal = OfferDismissal::Open,
    .rc = RcState::Absent,
    .slot = CardSlot::Free,
};
} // namespace

static_assert(ShellRows.size() == std::to_underlying(ShellKind::Fish));
static_assert(CardStageRows.size() == std::to_underlying(CardStage::Confirm) + 1U);

TEST_CASE("shellKind.rowsAreInEnumeratorOrderAndKnownToGenerateIntegration", "[shellintegration]")
{
    for (auto const [index, row]: core::views::enumerate(ShellRows))
    {
        CHECK(std::to_underlying(row.kind) == static_cast<std::size_t>(index) + 1U);
        CHECK(shellRow(row.kind) == &row);
        auto const known = std::ranges::any_of(contour::cli::supportedShells(),
                                               [&](auto const& shell) { return shell.name == row.name; });
        CHECK(known);
    }
    CHECK(shellRow(ShellKind::Unknown) == nullptr);
}

TEST_CASE("shellKind.shellKindOf", "[shellintegration]")
{
    struct Case
    {
        std::string_view name;
        ShellKind expected;
    };
    constexpr auto Cases = std::array {
        Case { "zsh", ShellKind::Zsh },       Case { "-zsh", ShellKind::Zsh },
        Case { "/bin/zsh", ShellKind::Zsh },  Case { "bash", ShellKind::Bash },
        Case { "-bash", ShellKind::Bash },    Case { "/usr/local/bin/fish", ShellKind::Fish },
        Case { "ssh", ShellKind::Unknown },   Case { "zsh5", ShellKind::Unknown },
        Case { "tmux", ShellKind::Unknown },  Case { "", ShellKind::Unknown },
        Case { "/bin/", ShellKind::Unknown }, Case { "--zsh", ShellKind::Unknown },
        Case { "sudo", ShellKind::Unknown },
    };
    for (auto const& c: Cases)
    {
        INFO(c.name);
        CHECK(shellKindOf(c.name) == c.expected);
    }
}

namespace
{
/// Feeds @p count commands that leave the verdict Waiting.
void enterCommands(ShellIntegrationProbe& probe, ScreenType screen, PromptMarks marks, int count)
{
    std::ranges::for_each(std::views::iota(0, count),
                          [&](int) { std::ignore = probe.noteCommandEntered(screen, marks); });
}
} // namespace

TEST_CASE("shellIntegrationProbe.offersOnTheThirdCommand", "[shellintegration]")
{
    auto probe = ShellIntegrationProbe {};
    CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Waiting);
    CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Waiting);
    CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Offer);
    CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Waiting);
}

TEST_CASE("shellIntegrationProbe.alternateScreenNeverCounts", "[shellintegration]")
{
    auto probe = ShellIntegrationProbe {};
    enterCommands(probe, Alternate, Absent, 10);
    CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Waiting);
    CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Waiting);
    CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Offer);
}

TEST_CASE("shellIntegrationProbe.aPromptMarkEndsTheProbe", "[shellintegration]")
{
    for (auto const before: std::views::iota(0, ShellIntegrationProbeCommands + 1))
    {
        INFO(before);
        auto probe = ShellIntegrationProbe {};
        enterCommands(probe, Primary, Absent, before);
        CHECK(probe.noteCommandEntered(Primary, PromptMarks::Seen) == ProbeVerdict::Integrated);
        // Even when the marks are gone again, it stays integrated, and never offers.
        CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Integrated);
        CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Integrated);
        CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Integrated);
        CHECK(probe.noteCommandEntered(Primary, Absent) == ProbeVerdict::Integrated);
    }
}

TEST_CASE("offerFor.everyFieldFlippedOnceGivesNone", "[shellintegration]")
{
    REQUIRE(offerFor(AllYes) == ShellIntegrationOffer::Card);

    auto shell = AllYes;
    shell.shell = ShellKind::Unknown;
    CHECK(offerFor(shell) == ShellIntegrationOffer::None);

    auto locality = AllYes;
    locality.locality = RcLocality::Remote;
    CHECK(offerFor(locality) == ShellIntegrationOffer::None);

    auto host = AllYes;
    host.host = HostPlatform::Windows;
    CHECK(offerFor(host) == ShellIntegrationOffer::None);

    auto dismissal = AllYes;
    dismissal.dismissal = OfferDismissal::Dismissed;
    CHECK(offerFor(dismissal) == ShellIntegrationOffer::None);

    auto rcUnknown = AllYes;
    rcUnknown.rc = RcState::Unknown;
    CHECK(offerFor(rcUnknown) == ShellIntegrationOffer::None);

    auto rcInstalled = AllYes;
    rcInstalled.rc = RcState::Installed;
    CHECK(offerFor(rcInstalled) == ShellIntegrationOffer::None);

    auto slot = AllYes;
    slot.slot = CardSlot::Taken;
    CHECK(offerFor(slot) == ShellIntegrationOffer::None);

    CHECK(offerFor({}) == ShellIntegrationOffer::None);
}

TEST_CASE("offerFor.everyNonWindowsHostOffers", "[shellintegration]")
{
    for (auto const host: { HostPlatform::Other, HostPlatform::MacOS, HostPlatform::KdePlasma })
    {
        auto in = AllYes;
        in.host = host;
        CHECK(offerFor(in) == ShellIntegrationOffer::Card);
    }
}

TEST_CASE("shellRcLine.quotesThePathForTheShellsSyntax", "[shellintegration]")
{
    constexpr auto Marker = " # Contour shell integration";
    CHECK(shellRcLine(ShellKind::Zsh, "/Applications/Contour.app/Contents/MacOS/contour").value()
          == "if [ \"$TERM_PROGRAM\" = contour ]; then eval \"$('/Applications/Contour.app/Contents/MacOS/"
             "contour' generate integration shell zsh to -)\"; fi # Contour shell integration");
    CHECK(shellRcLine(ShellKind::Bash, "/opt/contour").value()
          == "if [ \"$TERM_PROGRAM\" = contour ]; then eval \"$('/opt/contour' generate integration shell "
             "bash to -)\"; fi # Contour shell integration");
    CHECK(shellRcLine(ShellKind::Fish, "/opt/contour").value()
          == std::string("if test \"$TERM_PROGRAM\" = contour; '/opt/contour' generate integration shell "
                         "fish to - | source; end")
                 + Marker);

    CHECK(shellRcLine(ShellKind::Zsh, "/Applications/Contour Nightly.app/x")
              .value()
              .contains("'/Applications/Contour Nightly.app/x'"));
    CHECK(
        shellRcLine(ShellKind::Zsh, "/home/O'Brien/contour").value().contains("'/home/O'\\''Brien/contour'"));
    CHECK(
        shellRcLine(ShellKind::Fish, "/home/O'Brien/contour").value().contains("'/home/O\\'Brien/contour'"));
    CHECK(shellRcLine(ShellKind::Fish, "/home/a\\b/contour").value().contains("'/home/a\\\\b/contour'"));
    // A backslash is literal inside POSIX single quotes.
    CHECK(shellRcLine(ShellKind::Zsh, "/home/a\\b/contour").value().contains("'/home/a\\b/contour'"));
}

TEST_CASE("shellRcLine.everyLineEndsInTheMarkerAndHasNoLineBreak", "[shellintegration]")
{
    for (auto const& row: ShellRows)
    {
        auto const line = shellRcLine(row.kind, "/opt/contour").value();
        CHECK(line.ends_with(ShellRcMarker));
        CHECK(!line.contains('\n'));
        CHECK(line.contains(std::string("generate integration shell ") + std::string(row.name)));
    }
}

TEST_CASE("shellRcLine.refusesWhatItCannotWriteSafely", "[shellintegration]")
{
    CHECK(shellRcLine(ShellKind::Zsh, "contour").error() == ShellRcLineError::RelativePath);
    CHECK(shellRcLine(ShellKind::Zsh, "").error() == ShellRcLineError::RelativePath);
    CHECK(shellRcLine(ShellKind::Zsh, "./contour").error() == ShellRcLineError::RelativePath);
    for (auto const control: { '\n', '\r', '\t', '\0', '\x1b', '\x7f' })
    {
        auto path = std::string("/opt/con");
        path += control;
        path += "tour";
        INFO(static_cast<int>(control));
        CHECK(shellRcLine(ShellKind::Zsh, path).error() == ShellRcLineError::UnusablePath);
        CHECK(shellRcLine(ShellKind::Fish, path).error() == ShellRcLineError::UnusablePath);
    }
    CHECK(shellRcLine(ShellKind::Unknown, "/opt/contour").error() == ShellRcLineError::UnknownShell);
}

TEST_CASE("cardText.fillsPlaceholders", "[shellintegration]")
{
    auto const& offer = CardStageRows[std::to_underlying(CardStage::Offer)];
    CHECK(cardText(offer.body, "zsh", "~/.zshrc", "L")
          == "Install shell integration for zsh to fold output, see exit codes and jump between commands.");
    auto const& confirm = CardStageRows[std::to_underlying(CardStage::Confirm)];
    CHECK(cardText(confirm.title, "zsh", "~/.zshrc", "L") == "Add this line to ~/.zshrc?");
    CHECK(cardText(confirm.body, "zsh", "~/.zshrc", "echo {rc} {shell}") == "echo {rc} {shell}");
    for (auto const& row: CardStageRows)
        CHECK((row.stage == CardStage::Hidden) == row.title.empty());
}

TEST_CASE("displayRcPath.abbreviatesHome", "[shellintegration]")
{
    CHECK(displayRcPath("/Users/me/.zshrc", "/Users/me") == "~/.zshrc");
    CHECK(displayRcPath("/Users/me/.config/fish/config.fish", "/Users/me") == "~/.config/fish/config.fish");
    CHECK(displayRcPath("/etc/zshrc", "/Users/me") == "/etc/zshrc");
    CHECK(displayRcPath("/Users/me2/.zshrc", "/Users/me") == "/Users/me2/.zshrc");
    CHECK(displayRcPath("/Users/me/.zshrc", "") == "/Users/me/.zshrc");
}
