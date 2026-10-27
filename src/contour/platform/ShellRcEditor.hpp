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

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>

namespace contour::platform
{

/// Why a startup file could not be inspected or changed.
enum class ShellRcError : uint8_t
{
    NoHome = 0,   ///< `HOME` is not set, so no startup file can be named.
    Unreadable,   ///< The file exists and could not be read.
    Unwritable,   ///< The file could not be appended to; it is as it was.
    NotAFile,     ///< The path is a directory, a dangling link or something else but a regular file.
    Changed,      ///< The file changed between reading it and writing to it; nothing was written.
    InvalidLine,  ///< The line is empty or spans more than one line.
    PartialWrite, ///< The append was cut short, now or earlier: the file ends in an incomplete Contour line.
};

/// What @p error means, in words for a sentence that continues "was not changed: ".
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
        case ShellRcError::PartialWrite:
            return "its last line is a cut-short Contour line; delete that line and try again";
    }
    return "unknown error";
}

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
    /// @return The file and its state, or why it cannot be read.
    [[nodiscard]] virtual std::expected<RcInspection, ShellRcError> inspect(
        config::ShellKind shell) const = 0;

    /// Appends @p line to a shell's startup file, once.
    /// @param shell The shell.
    /// @param line The line, without a line ending.
    /// @return What was done, or why nothing was.
    [[nodiscard]] virtual std::expected<RcInstall, ShellRcError> install(config::ShellKind shell,
                                                                         std::string_view line) = 0;
};

/// An editor for a build or a test that must not reach any file: nothing is inspectable, and nothing
/// can be written.
class NullShellRcEditor final: public IShellRcEditor
{
  public:
    /// @return An inspection whose state is RcState::Unknown, so no offer follows from it.
    [[nodiscard]] std::expected<RcInspection, ShellRcError> inspect(
        config::ShellKind /*shell*/) const override
    {
        return RcInspection {};
    }

    /// @return Always ShellRcError::Unwritable.
    [[nodiscard]] std::expected<RcInstall, ShellRcError> install(config::ShellKind /*shell*/,
                                                                 std::string_view /*line*/) override
    {
        return std::unexpected(ShellRcError::Unwritable);
    }
};

/// What a startup file's text says about the integration.
/// @param shell The shell the file is for.
/// @param content The file's bytes.
/// @return Partial when the last line has no line ending and is the beginning of Contour's line (the
///         shell's config::ShellRow::guard) without its config::ShellRcMarker, which is what an append
///         cut short by a full disk leaves behind; a user's own unterminated last line is never that.
///         Else Installed when the file holds config::ShellRcMarker, or a line that is not a comment and
///         either runs `generate integration shell <name>` or sources `shell-integration.<name>`;
///         else Absent.
[[nodiscard]] config::RcState rcStateOf(config::ShellKind shell, std::string_view content);

/// The editor over a real (or injected) file system.
///
/// Locates the file from `HOME`, `ZDOTDIR` and `XDG_CONFIG_HOME`; a file that is a symlink is
/// appended through to its target. Appending reads the file again right before writing and refuses
/// when it changed meanwhile, and reads it once more afterwards: the file system's append may report
/// success for a write that was cut short (a close error nobody checked), so the file itself decides.
class FileShellRcEditor final: public IShellRcEditor
{
  public:
    /// @param fileSystem The file system the rc files live on; must outlive the editor.
    /// @param environment Where `HOME`, `ZDOTDIR` and `XDG_CONFIG_HOME` are read from; must outlive it.
    /// @param host The host, which decides bash's file.
    FileShellRcEditor(core::platform::FileSystem const& fileSystem,
                      core::Environment const& environment,
                      config::HostPlatform host);

    [[nodiscard]] std::expected<RcInspection, ShellRcError> inspect(config::ShellKind shell) const override;
    [[nodiscard]] std::expected<RcInstall, ShellRcError> install(config::ShellKind shell,
                                                                 std::string_view line) override;

  private:
    core::platform::FileSystem const& _fileSystem;
    core::Environment const& _environment;
    config::HostPlatform _host;
};

} // namespace contour::platform
