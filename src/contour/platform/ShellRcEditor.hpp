// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Adding the line that loads Contour's shell integration to a shell's startup file.
///
/// This writes a file users keep by hand, so it only ever appends: it never truncates, replaces or
/// renames the rc file, which keeps its permissions, its owner and, when it is a symlink into a
/// dotfiles repository, the link itself. Qt-free.

#include <contour/config/ShellKind.hpp>
#include <contour/config/WindowControlStyle.hpp>

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace contour::platform
{

/// Why a startup file could not be inspected or changed.
enum class ShellRcError : uint8_t
{
    NoHome = 0,     ///< `HOME` is not set, so no startup file can be named.
    Unreadable,     ///< The file exists and could not be read.
    Unwritable,     ///< The file could not be appended to; it is as it was.
    NotAFile,       ///< The path is a directory, a dangling link or something else but a regular file.
    Changed,        ///< The file changed between reading it and writing to it; nothing was written.
    InvalidLine,    ///< The line is empty or spans more than one line.
    PartialWrite,   ///< An earlier append was cut short: the file ends in an incomplete Contour line; nothing
                    ///< was written.
    Superuser,      ///< Contour runs as root, so a file it created in the user's home would belong to root.
    AppendCutShort, ///< This append was cut short: the file now ends in part of Contour's line.
    AppendLineEndingOnly, ///< This append was cut short before Contour's line: only a line ending landed.
    AppendUnterminated,   ///< This append landed all of Contour's line but its line ending.
};

/// @param error The failure.
/// @return Whether the failed install may have changed the file, so its text must not say it was not.
[[nodiscard]] constexpr bool changedFile(ShellRcError error) noexcept
{
    switch (error)
    {
        case ShellRcError::AppendCutShort:
        case ShellRcError::AppendLineEndingOnly:
        case ShellRcError::AppendUnterminated: return true;
        case ShellRcError::NoHome:
        case ShellRcError::Unreadable:
        case ShellRcError::Unwritable:
        case ShellRcError::NotAFile:
        case ShellRcError::Changed:
        case ShellRcError::InvalidLine:
        case ShellRcError::PartialWrite:
        case ShellRcError::Superuser: return false;
    }
    return false;
}

/// What @p error means, in words for a sentence that continues "{file} was not changed: " when
/// changedFile() is false, else "{file} ".
/// @param error The failure.
/// @return A short description.
[[nodiscard]] constexpr std::string_view describe(ShellRcError error) noexcept
{
    switch (error)
    {
        case ShellRcError::NoHome: return "the home directory is not known";
        case ShellRcError::Unreadable: return "the file could not be read";
        case ShellRcError::Unwritable: return "the file could not be written";
        case ShellRcError::NotAFile: return "it is not a regular file";
        case ShellRcError::Changed: return "it changed while it was being read";
        case ShellRcError::InvalidLine: return "the line to add is not a single line";
        case ShellRcError::Superuser: return "Contour is running as root";
        case ShellRcError::PartialWrite:
            return "its last line is a cut-short Contour line; delete that line and try again";
        case ShellRcError::AppendCutShort:
            return "now ends in a cut-short Contour line: the write stopped part-way; delete that last line "
                   "and try again";
        case ShellRcError::AppendLineEndingOnly:
            return "got a line ending but none of Contour's line: the write stopped before it; nothing "
                   "else changed, try again";
        case ShellRcError::AppendUnterminated:
            return "now holds Contour's line without its line ending: the write stopped at its very end; "
                   "add a line break after it";
    }
    return "unknown error";
}

/// Whose authority Contour runs with.
enum class RunningAs : uint8_t
{
    User = 0,  ///< An ordinary account.
    Superuser, ///< The superuser (effective uid 0).
};

/// @return Whom this process runs as: RunningAs::Superuser when its effective uid is 0, always
///         RunningAs::User where there is no such thing (Windows).
[[nodiscard]] RunningAs currentRunningAs() noexcept;

/// `ZDOTDIR`: the directory zsh reads its startup files from, else `HOME`.
inline constexpr std::string_view ZDotDirVariable = "ZDOTDIR";
/// `XDG_CONFIG_HOME`: the directory fish reads its configuration from, else `HOME/.config`.
inline constexpr std::string_view XdgConfigHomeVariable = "XDG_CONFIG_HOME";
/// The variables RcEnvironment may carry: every one a startup file's location depends on, beside `HOME`.
inline constexpr auto RcEnvironmentVariables = std::array { ZDotDirVariable, XdgConfigHomeVariable };

/// The variables a session's shell is started with that name where its startup file lives
/// (`ZDOTDIR`, `XDG_CONFIG_HOME`), by name. They win over Contour's own environment, because the shell
/// reads its files from where its own environment says. An empty value means "unset for that shell".
using RcEnvironment = std::map<std::string, std::string, std::less<>>;

/// What inspecting a shell's startup file found.
struct RcInspection
{
    std::filesystem::path file {};                    ///< The startup file, as the shell names it.
    config::RcState state = config::RcState::Unknown; ///< Whether it loads Contour's integration.

    [[nodiscard]] bool operator==(RcInspection const&) const = default;
};

/// What a successful install did.
enum class RcInstall : uint8_t
{
    Appended = 0,     ///< The line was added.
    AlreadyInstalled, ///< The file already loads the integration; nothing was written.
};

/// Reads and changes shells' startup files.
///
/// An interface per the dependency-injection principle: this is file I/O on files outside Contour's
/// own, so the GUI logic around it is tested against a recording double and never touches a real HOME.
class IShellRcEditor
{
  public:
    virtual ~IShellRcEditor() = default;

    /// Finds a shell's startup file and whether it loads the integration already.
    /// @param shell The shell.
    /// @param sessionEnvironment What the shell's session sets for it, where that is known.
    /// @return The file and its state, or why it cannot be read.
    [[nodiscard]] virtual std::expected<RcInspection, ShellRcError> inspect(
        config::ShellKind shell, RcEnvironment const& sessionEnvironment) const = 0;

    /// Appends @p line to a shell's startup file, once.
    /// @param shell The shell.
    /// @param line The line, without a line ending.
    /// @param sessionEnvironment What the shell's session sets for it, where that is known.
    /// @return What was done, or why nothing was.
    [[nodiscard]] virtual std::expected<RcInstall, ShellRcError> install(
        config::ShellKind shell, std::string_view line, RcEnvironment const& sessionEnvironment) = 0;
};

/// An editor for a build or a test that must not reach any file: nothing is inspectable, and nothing
/// can be written.
class NullShellRcEditor final: public IShellRcEditor
{
  public:
    /// @return An inspection whose state is RcState::Unknown, so no offer follows from it.
    [[nodiscard]] std::expected<RcInspection, ShellRcError> inspect(
        config::ShellKind /*shell*/, RcEnvironment const& /*sessionEnvironment*/) const override
    {
        return RcInspection {};
    }

    /// @return Always ShellRcError::Unwritable.
    [[nodiscard]] std::expected<RcInstall, ShellRcError> install(
        config::ShellKind /*shell*/,
        std::string_view /*line*/,
        RcEnvironment const& /*sessionEnvironment*/) override
    {
        return std::unexpected(ShellRcError::Unwritable);
    }
};

/// What a startup file's text says about the integration.
/// @param shell The shell the file is for.
/// @param content The file's bytes.
/// @return Partial when the last line has no line ending and is the beginning of Contour's line (the
///         shell's config::ShellRow::guard, then its config::ShellRow::continuation) without its
///         config::ShellRcMarker, which is what an append cut short by a full disk leaves behind; a user's
///         own unterminated last line is never that, not even one that opens with the same guard.
///         Else Installed when the file holds config::ShellRcMarker, or a line that is not a comment and
///         either runs `generate integration shell <name>` or sources `shell-integration.<name>`;
///         else Absent.
[[nodiscard]] config::RcState rcStateOf(config::ShellKind shell, std::string_view content);

/// The editor over a real (or injected) file system.
///
/// Locates the file from `HOME`, `ZDOTDIR` and `XDG_CONFIG_HOME`, each read from the session's own
/// environment where it sets one and else from Contour's; a file that is a symlink is appended through to
/// its target. It never runs the shell to ask: a `ZDOTDIR` that only `~/.zshenv` sets is therefore not
/// seen, and zsh then reads a `.zshrc` elsewhere than the one this appends to (the line is harmless but
/// does not load; the card names the file it wrote, so the user can move the line). A superuser's
/// Contour touches nothing: it would leave root-owned files in a user's home. Appending reads the file again
/// right before writing and refuses when it changed meanwhile, and reads it once more afterwards: the file
/// system's append may report success for a write that was cut short (a close error nobody checked), so the
/// file itself decides.
/// The re-read before writing narrows the window in which a concurrent editor's save can interfere but
/// cannot close it: a save in the instant between that check and the append can leave Contour's line glued
/// to the end of the user's last line. The check after the append reports that as an error (the file then
/// no longer equals what it was plus the line), never as success.
class FileShellRcEditor final: public IShellRcEditor
{
  public:
    /// @param fileSystem The file system the rc files live on; must outlive the editor.
    /// @param environment Where `HOME`, `ZDOTDIR` and `XDG_CONFIG_HOME` are read from; must outlive it.
    /// @param host The host, which decides bash's file.
    /// @param runningAs Whose authority Contour runs with; a superuser's editor refuses everything with
    ///        ShellRcError::Superuser. A value rather than a call to the OS, so a test needs no root.
    FileShellRcEditor(core::platform::FileSystem const& fileSystem,
                      core::Environment const& environment,
                      config::HostPlatform host,
                      RunningAs runningAs);

    [[nodiscard]] std::expected<RcInspection, ShellRcError> inspect(
        config::ShellKind shell, RcEnvironment const& sessionEnvironment) const override;
    [[nodiscard]] std::expected<RcInstall, ShellRcError> install(
        config::ShellKind shell, std::string_view line, RcEnvironment const& sessionEnvironment) override;

  private:
    core::platform::FileSystem const& _fileSystem;
    core::Environment const& _environment;
    config::HostPlatform _host;
    RunningAs _runningAs;
};

} // namespace contour::platform
