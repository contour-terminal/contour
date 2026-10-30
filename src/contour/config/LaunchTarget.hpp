// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a window's sessions live, chosen before the window opens: the one grammar `contour connect`, the
/// picker, the launchers' menus and the last-used memory all speak. Qt-free, and a table: a new kind of
/// target is a row of @ref LaunchKeywords, or of @ref LaunchTargetKinds.

#include <contour/config/HostUrl.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace contour::config
{

/// The kinds of place a window's sessions can live.
enum class LaunchTargetKind : uint8_t
{
    LocalDaemon = 0, ///< This computer's daemon, whose sessions outlive the window (`daemon[:LABEL]`).
    InProcess,       ///< This window only: no daemon, its shells end when it closes (`local`).
    SavedHost,       ///< A host of `hosts:`, by its name.
    Address,         ///< A host typed as an `ssh://` or `tls://` URL, not saved.
};

/// A chosen place, as its text spells it. Equal targets spell alike: an address is kept canonical.
struct LaunchTarget
{
    LaunchTargetKind kind = LaunchTargetKind::LocalDaemon; ///< What kind of place it is.
    /// The daemon's socket label (empty: the default one), the saved host's name, or the address in its
    /// canonical spelling; empty for @ref LaunchTargetKind::InProcess.
    std::string name {};

    [[nodiscard]] bool operator==(LaunchTarget const&) const = default;
};

/// Whether a keyword may carry a `:LABEL`.
enum class KeywordLabel : uint8_t
{
    None = 0, ///< The keyword stands alone.
    Optional, ///< `keyword` or `keyword:LABEL`.
};

/// One row of the keyword table: a word that names a kind of target by itself.
struct LaunchKeyword
{
    std::string_view keyword; ///< The word, lower case.
    LaunchTargetKind kind;    ///< What it names.
    KeywordLabel label;       ///< Whether `:LABEL` may follow.
};

/// Every keyword a target may begin with; they are reserved, so a saved host named `local` is reached by
/// the picker, not by this grammar. Adding a keyword is adding a row.
inline constexpr auto LaunchKeywords = std::array {
    LaunchKeyword { "daemon", LaunchTargetKind::LocalDaemon, KeywordLabel::Optional },
    LaunchKeyword { "local", LaunchTargetKind::InProcess, KeywordLabel::None },
};

/// Why a target text was refused.
enum class LaunchTargetError : uint8_t
{
    Empty = 0,    ///< No text at all.
    UnknownName,  ///< Neither a keyword, a URL, nor a saved host.
    BadAddress,   ///< A `scheme://` text whose address does not parse.
    LabelNotUsed, ///< A `:LABEL` after a keyword that takes none.
    EmptyLabel,   ///< `daemon:` with nothing after the colon.
};

/// A refused target text and what to tell the user about it.
struct LaunchTargetFailure
{
    LaunchTargetError error = LaunchTargetError::Empty; ///< Why.
    std::string detail {};                              ///< The text, or the address's own refusal.

    /// @return A sentence saying what is wrong, for the user who wrote the target.
    [[nodiscard]] std::string describe() const;
};

/// Parses a target.
/// @param text A keyword (`daemon`, `daemon:LABEL`, `local`), a saved host's name, or an `ssh://` / `tls://`
///             URL.
/// @param hosts The saved hosts, which a bare name is looked up in.
/// @return The target, or why @p text names none.
[[nodiscard]] std::expected<LaunchTarget, LaunchTargetFailure> parseLaunchTarget(
    std::string_view text, std::map<std::string, HostEntry> const& hosts);

/// @param target A target.
/// @return Its text, which parseLaunchTarget() reads back: `daemon`, `daemon:build`, `local`, `starfive`,
///         `ssh://me@pi4.lan`. What the last-used memory stores and `contour connect --list` prints.
[[nodiscard]] std::string toString(LaunchTarget const& target);

/// What a target asks of the attach machinery `contour client` and `contour terminal` already are.
enum class Hosting : uint8_t
{
    InProcess = 0, ///< `contour terminal --daemon-mode in_process`.
    LocalDaemon,   ///< `contour client [--label LABEL]`.
    RemoteHost,    ///< `contour client --host NAME|URL`.
};

/// A target mapped onto an attach request.
struct AttachPlan
{
    Hosting hosting = Hosting::InProcess; ///< Which attach path.
    std::string label {};                 ///< The daemon's socket label, for Hosting::LocalDaemon.
    std::string host {};                  ///< The `--host` value, for Hosting::RemoteHost.

    [[nodiscard]] bool operator==(AttachPlan const&) const = default;
};

/// @param target A target.
/// @return The attach request it stands for.
[[nodiscard]] AttachPlan attachPlanOf(LaunchTarget const& target);

/// The command line that opens a window on @p target through the connect verb: what a launcher's menu entry
/// and the picker run, one process per window as a GUI process attaches to exactly one daemon.
/// @param target The target.
/// @param configPath The configuration file the new process loads; empty for the default one.
/// @return The arguments after the program: `connect [--config FILE] -- TARGET`.
[[nodiscard]] std::vector<std::string> connectArguments(LaunchTarget const& target,
                                                        std::string_view configPath = {});

} // namespace contour::config
