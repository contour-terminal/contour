// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Overloaded.hpp>
#include <core/platform/Clock.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace vtpty
{

/// How long Process::close() gives a child that the closing terminal hung up to end, before it kills
/// the child; on POSIX only, as on Windows closing the pseudoconsole ends the child outright.
///
/// Nothing tells the grace that the child has ended -- nobody need be waiting for it -- so it asks:
/// whether the child has ended, by waitpid(), and whether the grace has passed, by @ref clock, about
/// every 10 ms. It ends at whichever comes first.
struct HangupGrace
{
    /// wezterm's grace for the same hangup.
    static constexpr auto Default = std::chrono::milliseconds { 200 };

    std::chrono::milliseconds duration = Default; ///< How long the child is given.

    /// The clock the grace is measured on. It must outlive the Process.
    std::reference_wrapper<core::platform::IClock const> clock = core::platform::defaultSteadyClock();
};

/**
 * Spawns and manages a child process with a pseudo terminal attached to it.
 */
class [[nodiscard]] Process: public Pty
{
  public:
    // clang-format off
    struct NormalExit { int exitCode; };
    struct SignalExit { int signum; };
    // clang-format on

    using ExitStatus = std::variant<NormalExit, SignalExit>;
    using Environment = std::map<std::string, std::string>;

    struct ExecInfo
    {
        std::string program;
        std::vector<std::string> arguments;
        std::filesystem::path workingDirectory;
        Environment env;
        /// Variables dropped from the inherited environment (an `env` entry of the same name still applies).
        std::vector<std::string> removedEnvironment;
    };

    //! Returns login shell of current user.
    static std::vector<std::string> loginShell(bool escapeSandbox);

    static std::string userName();
    static std::filesystem::path homeDirectory();

    /// Overlays @p commandOverride onto @p shell, in place: the shared rule for a layout pane's
    /// per-session command override, applied identically by every PTY factory
    /// (AppSessionFactory::createPty and vthost::Daemon's makeShellPtyFactory) so the local-GUI and
    /// daemon-hosted startup-layout paths cannot drift apart.
    ///
    /// The program/arguments are replaced only when @p commandOverride carries a non-empty program:
    /// an engaged override with an empty program (a directory-only layout pane) must not wipe the
    /// shell's configured default arguments. The working directory is overlaid independently of the
    /// program, so a directory-only pane can still redirect the shell's cwd.
    ///
    /// The override's environment entries are laid over the shell's, an entry of the same name winning.
    /// @param shell            The shell config to overlay onto; modified in place.
    /// @param commandOverride  A session's command override, if any; no-op when nullopt.
    static void applyCommandOverride(ExecInfo& shell, std::optional<ExecInfo> const& commandOverride)
    {
        if (!commandOverride)
            return;
        if (!commandOverride->program.empty())
        {
            shell.program = commandOverride->program;
            shell.arguments = commandOverride->arguments;
        }
        if (!commandOverride->workingDirectory.empty())
            shell.workingDirectory = commandOverride->workingDirectory;
        for (auto const& [name, value]: commandOverride->env)
            shell.env.insert_or_assign(name, value);
    }

    /// @param placement Moves each spawned child into its own resource domain before it runs; never
    ///                  null. @see ProcessPlacement.
    /// @param hangupGrace How long close() gives the child to end on the hangup before killing it.
    Process(ExecInfo const& exe,
            std::unique_ptr<Pty> pty,
            bool escapeSandbox,
            std::shared_ptr<ProcessPlacement> placement,
            HangupGrace hangupGrace = {}):
        Process(exe.program,
                exe.arguments,
                exe.workingDirectory,
                exe.env,
                exe.removedEnvironment,
                escapeSandbox,
                std::move(pty),
                std::move(placement),
                hangupGrace)
    {
    }

    /// @param removedEnvironment Variables the child must not inherit.
    /// @param placement Moves each spawned child into its own resource domain before it runs; never
    ///                  null. @see ProcessPlacement.
    /// @param hangupGrace How long close() gives the child to end on the hangup before killing it.
    Process(std::string const& path,
            std::vector<std::string> const& args,
            std::filesystem::path const& cwd,
            Environment const& env,
            std::vector<std::string> removedEnvironment,
            bool escapeSandbox,
            std::unique_ptr<Pty> pty,
            std::shared_ptr<ProcessPlacement> placement,
            HangupGrace hangupGrace = {});

    ~Process() override;

    // Tests if the current process is running inside flatpak.
    static bool isFlatpak();

    // A Process has a child only once start() has spawned one. Before that, and for good when start()
    // failed before spawning, there is nothing to ask about, wait for or signal, and the calls below
    // say so rather than reaching for whatever process an unset id would name.

    /// @return Whether the child is running: false before start() spawned one, and after it exited.
    [[nodiscard]] bool alive() const noexcept;

    /// @return The child's exit status once it has exited; nullopt while it runs, or when there is none.
    [[nodiscard]] std::optional<ExitStatus> checkStatus() const;

    /// Waits for the child to exit.
    /// @return Its exit status, or nullopt when start() never spawned a child to wait for.
    [[nodiscard]] std::optional<ExitStatus> wait();

    [[nodiscard]] std::string workingDirectory() const;

    /// The name of the program leading the terminal's foreground process group: the shell at its prompt,
    /// the program it runs otherwise. Linux and macOS only.
    /// @return The name, as the operating system shortens it; none without a child, on another platform, or
    ///         on a Pty that is not a UnixPty.
    [[nodiscard]] std::optional<std::string> foregroundProcessName() const override;

    /// How terminate() asks the child to end.
    enum class TerminationHint : uint8_t
    {
        Normal, ///< An ordinary termination (SIGTERM), which the child may catch or ignore.
        Hangup, ///< As a closed terminal would (SIGHUP), which the child may catch or ignore.
        Kill,   ///< Outright (SIGKILL), which no child can refuse.
    };
    /// Asks the child to end, or does nothing when there is no child running. On Windows, which has no
    /// signals, every hint ends it outright.
    /// @param terminationHint How to ask.
    void terminate(TerminationHint terminationHint);

    [[nodiscard]] Pty& pty() noexcept;
    [[nodiscard]] Pty const& pty() const noexcept;

    // Pty overrides
    // clang-format off
    [[nodiscard]] StartResult start() override;
    [[nodiscard]] PtySlave& slave() noexcept override { return pty().slave(); }
    /// Closes the child's terminal, which hangs it up. On POSIX a child that ignores the hangup is killed
    /// once its HangupGrace has passed, so that waiting for it -- waitForClosed(), ~Process -- ends.
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override { return pty().isClosed(); }
    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage, std::optional<std::chrono::milliseconds> timeout, size_t n) override { return pty().read(storage, timeout, n); }
    void wakeupReader() override { pty().wakeupReader(); }
    [[nodiscard]] int write(std::string_view data) override { return pty().write(data); }
    [[nodiscard]] PageSize pageSize() const noexcept override { return pty().pageSize(); }
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override { pty().resizeScreen(cells, pixels); }
    // clang-format on

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};

} // namespace vtpty

template <>
struct std::formatter<vtpty::Process::ExitStatus>: std::formatter<std::string>
{
    auto format(vtpty::Process::ExitStatus const& status, auto& ctx) const
    {
        auto const text =
            std::visit(core::Overloaded { [&](vtpty::Process::NormalExit exit) {
                                             return std::format("{} (normal exit)", exit.exitCode);
                                         },
                                          [&](vtpty::Process::SignalExit exit) {
                                              char buf[256];
#ifdef _WIN32
                                              strerror_s(buf, sizeof(buf), errno);
                                              return std::format("{} (signal number {})", buf, exit.signum);
#else
                                              return std::format("{} (signal number {})",
                                                                 strerror_r(errno, buf, sizeof(buf)),
                                                                 exit.signum);
#endif
                                          } },
                       status);
        return std::formatter<std::string>::format(text, ctx);
    }
};
