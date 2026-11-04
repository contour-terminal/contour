// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the Settings Shell Integration page says about each shell's startup file, and whether it offers to
/// install. Pure: SettingsController feeds it what the rc editor found, and the tests read the same table.

#include <contour/config/ShellKind.hpp>
#include <contour/platform/ShellRcEditor.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace contour::window
{

/// Whether a row's Install... button is offered.
enum class InstallOffer : uint8_t
{
    No = 0, ///< Nothing to install, or the editor would refuse.
    Yes,    ///< The startup file lacks the line and can be appended to, or loads it under a policy to fix.
};

/// What a row says instead when the shell's execution policy keeps its profile from running.
struct ShellPolicyStatus
{
    config::ProfileScripts scripts = config::ProfileScripts::Run; ///< The policy's verdict it is for.
    std::string_view text {};                                     ///< The status pattern.
    InstallOffer install = InstallOffer::No; ///< Whether Install... is offered under that verdict.
};

/// A profile that loads Contour already: what each verdict that stops it says. Install then only fixes the
/// policy, where the user may.
inline constexpr auto InstalledUnderPolicy = std::array {
    ShellPolicyStatus { .scripts = config::ProfileScripts::NeedUserPolicy,
                        .text = "Installed in {rc}, but PowerShell's script policy stops it from running",
                        .install = InstallOffer::Yes },
    ShellPolicyStatus { .scripts = config::ProfileScripts::BlockedByGroupPolicy,
                        .text = InstallBlockRows[std::to_underlying(InstallBlock::GroupPolicy)].text,
                        .install = InstallOffer::No },
    ShellPolicyStatus { .scripts = config::ProfileScripts::SignedOnly,
                        .text = InstallBlockRows[std::to_underlying(InstallBlock::SignedScriptsOnly)].text,
                        .install = InstallOffer::No },
};

/// What a shell's startup file is, as a row says it. The pattern holds `{rc}`, the file as the user would
/// say it, and `{why}`, what the editor said was wrong.
struct ShellStatusRow
{
    config::RcState state = config::RcState::Unknown; ///< The state the row is for.
    std::string_view text {};                         ///< The status pattern.
    InstallOffer install = InstallOffer::No;          ///< Whether Install... is offered in this state.
    /// What replaces @c text and @c install under an execution policy that keeps the profile from running;
    /// a verdict not listed changes nothing here (an install's own block is installBlockOf()'s).
    std::span<ShellPolicyStatus const> underPolicy {};
};

/// A status for every state, in enumerator order.
inline constexpr auto ShellStatusRows = std::array {
    ShellStatusRow { .state = config::RcState::Unknown, .text = "Unknown", .install = InstallOffer::No },
    ShellStatusRow {
        .state = config::RcState::Absent, .text = "Not installed", .install = InstallOffer::Yes },
    ShellStatusRow { .state = config::RcState::Installed,
                     .text = "Installed in {rc}",
                     .install = InstallOffer::No,
                     .underPolicy = InstalledUnderPolicy },
    // Install would be refused (PartialWrite) until the user deletes the cut line, which the row names.
    ShellStatusRow { .state = config::RcState::Partial,
                     .text = "Cut short in {rc}: delete this line, then install again",
                     .install = InstallOffer::No },
};

/// What the page shows for a shell whose startup file could not be inspected.
inline constexpr std::string_view ShellUnknownBecause = "Unknown: {why}";

/// One shell's row on the page.
struct ShellInstallRow
{
    config::ShellKind shell = config::ShellKind::Unknown; ///< The shell.
    std::string name {};                                  ///< Its name, as the row is titled.
    std::string status {};                                ///< What its startup file is.
    std::string rc {};         ///< The startup file as the user would say it; empty when unknown.
    std::string deleteLine {}; ///< The cut-short line the user must delete; empty unless Partial.
    InstallOffer install = InstallOffer::No; ///< Whether Install... is offered.
};

/// Fills a status pattern in.
/// @param pattern Text holding `{rc}` and `{why}` placeholders.
/// @param rc The startup file.
/// @param why What went wrong.
/// @return The text; substituted text is never scanned again.
[[nodiscard]] inline std::string statusText(std::string_view pattern,
                                            std::string_view rc,
                                            std::string_view why)
{
    auto text = std::string {};
    while (!pattern.empty())
    {
        if (pattern.starts_with("{rc}"))
        {
            text += rc;
            pattern.remove_prefix(4);
        }
        else if (pattern.starts_with("{why}"))
        {
            text += why;
            pattern.remove_prefix(5);
        }
        else
        {
            text += pattern.front();
            pattern.remove_prefix(1);
        }
    }
    return text;
}

/// The row of a shell.
/// @param shell The shell.
/// @param inspection What the rc editor found for it.
/// @param home The home directory, which a startup file under it is shown relative to as `~`.
/// @param host The host, which with @p binary and the inspected execution policy decides what blocks Install
///        (installBlockOf()): the same decision the card asks.
/// @param binary Whether the running binary can be named by an rc line.
/// @return The row, titled with the shell's display name: its status, the file, the line to delete for a
///         cut-short install, and whether to offer the install. The state's row decides, as the execution
///         policy's verdict changes it (ShellStatusRow::underPolicy); a block then turns an installable row's
///         status into the block's words.
[[nodiscard]] inline ShellInstallRow shellInstallRowOf(
    config::ShellRow const& shell,
    std::expected<platform::RcInspection, platform::ShellRcError> const& inspection,
    std::filesystem::path const& home,
    config::HostPlatform host,
    BinaryReach binary)
{
    auto row = ShellInstallRow { .shell = shell.kind, .name = std::string { shell.displayName } };
    if (!inspection)
    {
        row.status = statusText(ShellUnknownBecause, "", platform::describe(inspection.error()));
        return row;
    }
    auto const& status = ShellStatusRows[static_cast<std::size_t>(inspection->state)];
    auto const policy =
        std::ranges::find(status.underPolicy, inspection->scripts, &ShellPolicyStatus::scripts);
    auto const underPolicy = policy != status.underPolicy.end();
    row.rc = inspection->file.empty() ? std::string {} : displayRcPath(inspection->file, home);
    row.status = statusText(underPolicy ? policy->text : status.text, row.rc, "");
    row.install = underPolicy ? policy->install : status.install;
    if (auto const block = installBlockOf(shell, host, binary, inspection->scripts);
        row.install == InstallOffer::Yes && block != InstallBlock::None)
    {
        row.install = InstallOffer::No;
        row.status = std::string { InstallBlockRows[std::to_underlying(block)].text };
    }
    if (inspection->state == config::RcState::Partial)
        row.deleteLine = inspection->partialLine;
    return row;
}

/// The row of a shell whose startup file is not looked at, because the host alone rules Install out.
/// @param shell The shell.
/// @param block What rules it out.
/// @return The row, titled with the shell's display name, saying the block's words and offering no install.
[[nodiscard]] inline ShellInstallRow blockedShellInstallRowOf(config::ShellRow const& shell,
                                                              InstallBlock block)
{
    return ShellInstallRow { .shell = shell.kind,
                             .name = std::string { shell.displayName },
                             .status = std::string { InstallBlockRows[std::to_underlying(block)].text },
                             .install = InstallOffer::No };
}

} // namespace contour::window
