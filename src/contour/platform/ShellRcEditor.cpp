// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ShellRcEditor.hpp>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace contour::platform
{

namespace
{
    /// A startup file other than the shell's own that, when present, means the integration is loaded.
    struct AlternateInstall
    {
        config::ShellKind shell = config::ShellKind::Unknown;
        std::string_view file {}; ///< Relative to the same base as the shell's rc file.
    };

    /// fish loads every file in `conf.d`, so a `contour.fish` there is the documented manual install.
    constexpr auto AlternateInstalls = std::array {
        AlternateInstall { .shell = config::ShellKind::Fish, .file = "fish/conf.d/contour.fish" },
    };

    /// Contour's environment with what the shell's session sets laid over it.
    class SessionEnvironment final: public core::Environment
    {
      public:
        SessionEnvironment(core::Environment const& base, RcEnvironment const& session):
            _base { base }, _session { session }
        {
        }

        [[nodiscard]] std::optional<std::string> get(std::string_view name) const override
        {
            if (auto const found = _session.find(name); found != _session.end())
                return found->second;
            return _base.get(name);
        }

      private:
        core::Environment const& _base;
        RcEnvironment const& _session;
    };

    /// @return Whether @p path is set, absolute and so usable as the root of a startup file's path.
    [[nodiscard]] std::optional<std::filesystem::path> absoluteVariable(core::Environment const& environment,
                                                                        std::string_view name)
    {
        auto value = environment.get(name);
        if (!value || value->empty())
            return std::nullopt;
        auto path = std::filesystem::path { *std::move(value) };
        if (!path.is_absolute())
            return std::nullopt;
        return path;
    }

    /// The directory @p base names, per R-e: `ZDOTDIR` else `HOME`; `XDG_CONFIG_HOME` else `HOME/.config`.
    [[nodiscard]] std::expected<std::filesystem::path, ShellRcError> baseDirectory(
        core::Environment const& environment, config::RcBase base)
    {
        auto const home = absoluteVariable(environment, "HOME");
        switch (base)
        {
            case config::RcBase::Home:
                if (!home)
                    return std::unexpected(ShellRcError::NoHome);
                return *home;
            case config::RcBase::ZDotDir:
                if (auto zdotdir = absoluteVariable(environment, ZDotDirVariable))
                    return *std::move(zdotdir);
                if (!home)
                    return std::unexpected(ShellRcError::NoHome);
                return *home;
            case config::RcBase::XdgConfig:
                if (auto xdg = absoluteVariable(environment, XdgConfigHomeVariable))
                    return *std::move(xdg);
                if (!home)
                    return std::unexpected(ShellRcError::NoHome);
                return *home / ".config";
        }
        return std::unexpected(ShellRcError::NoHome);
    }

    /// A startup file as it stands.
    struct RcFile
    {
        std::filesystem::path path {};     ///< As the shell names it.
        std::filesystem::path resolved {}; ///< Where writes go: the target when @c path is a symlink.
        std::string content {};            ///< Empty when the file does not exist.
        bool exists = false;
    };

    [[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
    {
        constexpr auto Blanks = std::string_view { " \t\r" };
        auto const first = text.find_first_not_of(Blanks);
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(Blanks) - first + 1);
    }

    /// Reads @p path, which is a startup file or may yet become one.
    [[nodiscard]] std::expected<RcFile, ShellRcError> readRc(core::platform::FileSystem const& fileSystem,
                                                             std::filesystem::path path)
    {
        auto file = RcFile { .path = std::move(path) };
        file.resolved = fileSystem.weaklyCanonical(file.path);
        if (fileSystem.isSymlink(file.path) && !fileSystem.isRegularFile(file.path))
            return std::unexpected(ShellRcError::NotAFile); // a dangling link, or one to a directory
        file.exists = fileSystem.exists(file.resolved);
        if (!file.exists)
            return file;
        if (!fileSystem.isRegularFile(file.resolved))
            return std::unexpected(ShellRcError::NotAFile);
        auto content = fileSystem.readFile(file.resolved);
        if (!content)
            return std::unexpected(ShellRcError::Unreadable);
        file.content = *std::move(content);
        return file;
    }

    /// The startup file of @p row.
    [[nodiscard]] std::expected<std::filesystem::path, ShellRcError> rcPath(
        core::Environment const& environment, config::ShellRow const& row, config::HostPlatform host)
    {
        auto const base = baseDirectory(environment, row.base);
        if (!base)
            return std::unexpected(base.error());
        return *base / (host == config::HostPlatform::MacOS ? row.rcFileMacOS : row.rcFile);
    }

    /// Whether the shell's own alternate file loads the integration.
    [[nodiscard]] bool hasAlternateInstall(core::platform::FileSystem const& fileSystem,
                                           core::Environment const& environment,
                                           config::ShellRow const& row)
    {
        auto const base = baseDirectory(environment, row.base);
        if (!base)
            return false;
        return std::ranges::any_of(AlternateInstalls, [&](AlternateInstall const& alternate) {
            return alternate.shell == row.kind && fileSystem.exists(*base / alternate.file);
        });
    }

    /// Whether @p content's last line has no line ending and only begins Contour's line for @p row.
    [[nodiscard]] bool endsInPartialLine(config::ShellRow const& row, std::string_view content) noexcept
    {
        if (content.empty() || content.ends_with('\n'))
            return false;
        auto const lastLine = trimmed(content.substr(content.find_last_of('\n') + 1));
        if (lastLine.empty() || lastLine.ends_with(config::ShellRcMarker))
            return false;
        // A cut anywhere in the guard is the guard's prefix. Past it the guard alone proves nothing, since a
        // user may open a line of their own the same way: Contour's continuation must follow (or be cut).
        if (row.guard.starts_with(lastLine))
            return true;
        if (!lastLine.starts_with(row.guard))
            return false;
        auto const rest = lastLine.substr(row.guard.size());
        return row.continuation.starts_with(rest) || rest.starts_with(row.continuation);
    }
} // namespace

RunningAs currentRunningAs() noexcept
{
#ifdef _WIN32
    return RunningAs::User;
#else
    return geteuid() == 0 ? RunningAs::Superuser : RunningAs::User;
#endif
}

config::RcState rcStateOf(config::ShellKind shell, std::string_view content)
{
    auto const* const row = config::shellRow(shell);
    if (row == nullptr)
        return config::RcState::Absent;
    if (endsInPartialLine(*row, content))
        return config::RcState::Partial;
    if (content.contains(config::ShellRcMarker))
        return config::RcState::Installed;

    auto const generates = std::format("generate integration shell {}", row->name);
    auto const script = std::format("shell-integration.{}", row->name);
    auto const loads = [&](std::string_view rawLine) {
        auto const line = trimmed(rawLine);
        if (line.empty() || line.starts_with('#'))
            return false;
        return line.contains(generates)
               || (line.contains(script) && (line.contains("source") || line.starts_with(". ")));
    };
    auto lines = std::views::split(content, '\n');
    auto const found = std::ranges::any_of(
        lines, [&](auto const& line) { return loads(std::string_view { line.begin(), line.end() }); });
    return found ? config::RcState::Installed : config::RcState::Absent;
}

FileShellRcEditor::FileShellRcEditor(core::platform::FileSystem const& fileSystem,
                                     core::Environment const& environment,
                                     config::HostPlatform host,
                                     RunningAs runningAs):
    _fileSystem { fileSystem }, _environment { environment }, _host { host }, _runningAs { runningAs }
{
}

std::expected<RcInspection, ShellRcError> FileShellRcEditor::inspect(
    config::ShellKind shell, RcEnvironment const& sessionEnvironment) const
{
    auto const* const row = config::shellRow(shell);
    if (row == nullptr)
        return RcInspection {};
    if (_runningAs == RunningAs::Superuser)
        return std::unexpected(ShellRcError::Superuser);
    auto const environment = SessionEnvironment { _environment, sessionEnvironment };
    auto const path = rcPath(environment, *row, _host);
    if (!path)
        return std::unexpected(path.error());
    auto const file = readRc(_fileSystem, *path);
    if (!file)
        return std::unexpected(file.error());
    auto state = rcStateOf(shell, file->content);
    if (state == config::RcState::Absent && hasAlternateInstall(_fileSystem, environment, *row))
        state = config::RcState::Installed;
    return RcInspection { .file = file->path, .state = state };
}

std::expected<RcInstall, ShellRcError> FileShellRcEditor::install(config::ShellKind shell,
                                                                  std::string_view line,
                                                                  RcEnvironment const& sessionEnvironment)
{
    auto const* const row = config::shellRow(shell);
    if (row == nullptr)
        return std::unexpected(ShellRcError::Unwritable);
    if (_runningAs == RunningAs::Superuser)
        return std::unexpected(ShellRcError::Superuser);
    if (line.empty() || line.find_first_of("\r\n") != std::string_view::npos)
        return std::unexpected(ShellRcError::InvalidLine);

    auto const environment = SessionEnvironment { _environment, sessionEnvironment };
    auto const path = rcPath(environment, *row, _host);
    if (!path)
        return std::unexpected(path.error());
    auto const before = readRc(_fileSystem, *path);
    if (!before)
        return std::unexpected(before.error());
    auto const state = rcStateOf(shell, before->content);
    if (state == config::RcState::Partial)
        return std::unexpected(ShellRcError::PartialWrite); // appending would only make the parse worse
    if (state == config::RcState::Installed || hasAlternateInstall(_fileSystem, environment, *row))
        return RcInstall::AlreadyInstalled;

    // Parent directories first: a fish user may have no config directory yet. Creating a directory
    // changes nothing the user wrote.
    if (auto const parent = before->resolved.parent_path();
        !parent.empty() && !_fileSystem.isDirectory(parent))
        if (!_fileSystem.createDirectories(parent))
            return std::unexpected(ShellRcError::Unwritable);

    // Refuse, rather than append to a file somebody else just edited: the decision above was about
    // the bytes read first.
    auto const right = readRc(_fileSystem, *path);
    if (!right)
        return std::unexpected(right.error());
    if (right->exists != before->exists || right->content != before->content)
        return std::unexpected(ShellRcError::Changed);

    // A file that does not end in a line ending gets one first, so the line starts on its own line.
    // CRLF files keep their endings: only the line this adds ends in a bare LF, which every shell reads.
    auto chunk = std::string {};
    if (!before->content.empty() && !before->content.ends_with('\n'))
        chunk += '\n';
    chunk += line;
    chunk += '\n';

    // The append's own verdict is not trusted: it can report success for a write whose tail never reached
    // the disk. Nothing is truncated or restored either way: an append can only have added bytes, so the
    // user's own lines are intact. The file, read again, says what landed.
    std::ignore = _fileSystem.appendFile(before->resolved, chunk);
    auto const after = readRc(_fileSystem, *path);
    if (after && after->content == before->content + chunk)
        return RcInstall::Appended;
    if (after && after->content != before->content)
        return std::unexpected(ShellRcError::PartialWrite);
    return std::unexpected(ShellRcError::Unwritable);
}

} // namespace contour::platform
