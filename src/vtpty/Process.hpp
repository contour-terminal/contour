// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/AdoptedChild.hpp>
#include <vtpty/ChildProcess.hpp>
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/InheritedPty.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/ProgramLookup.hpp>
#include <vtpty/Pty.hpp>

#include <crispy/BufferObject.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vtpty
{

/// Whether a spawn can start its program in a directory held open.
enum class HeldDirectorySpawn : uint8_t
{
    Unsupported = 0, ///< It cannot: the spawn would have to name the path, which is what the handle replaces.
    Supported,       ///< It can: the trampoline enters the handle.
};

/// @param host Where a spawn's program runs.
/// @return Whether it can be started in a directory held open: only on this machine, where the trampoline is
///         handed the descriptor; flatpak-spawn could only be handed the path.
[[nodiscard]] constexpr HeldDirectorySpawn heldDirectorySpawnOn(ProgramHost host) noexcept
{
    return host == ProgramHost::ThisMachine ? HeldDirectorySpawn::Supported : HeldDirectorySpawn::Unsupported;
}

/**
 * Spawns and manages a child process with a pseudo terminal attached to it.
 */
class [[nodiscard]] Process: public Pty, public ChildProcess
{
  public:
    // Member aliases of the types ChildProcess speaks, so that callers of Process keep naming them here.
    using NormalExit = vtpty::NormalExit;
    using SignalExit = vtpty::SignalExit;
    using ExitStatus = vtpty::ExitStatus;
    using SpawnProgress = vtpty::SpawnProgress;
    using TerminationHint = vtpty::TerminationHint;
    using Environment = std::map<std::string, std::string>;

    struct ExecInfo
    {
        std::string program;
        std::vector<std::string> arguments;
        std::filesystem::path workingDirectory;
        Environment env;
        /// Variables dropped from the inherited environment (an `env` entry of the same name still applies).
        std::vector<std::string> removedEnvironment;
        /// The directory the child starts in, held open and entered by its descriptor (POSIX: the
        /// trampoline's fchdir()). Set, workingDirectory is only what the process reports as its directory
        /// until the child has settled; it is never resolved again. A spawn that cannot enter it by
        /// descriptor fails.
        ///
        /// Windows, where no CreateProcess takes a directory by handle: the child is started by its resolved
        /// path, verified by file id before and after creation and guarded against a rename or removal while
        /// the child runs; refused when renamed since it was opened. The program is the .exe found on the
        /// absolute entries of the child's own PATH, and the child runs in a kill-on-close job of its own.
        std::shared_ptr<DirectoryHandle const> directory {};
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
    /// The override's environment entries are laid over the shell's, an entry of the same name winning; the
    /// names it withholds are withheld too, and a directory it holds open is the one the child enters.
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
        shell.removedEnvironment.insert(shell.removedEnvironment.end(),
                                        commandOverride->removedEnvironment.begin(),
                                        commandOverride->removedEnvironment.end());
        if (commandOverride->directory)
            shell.directory = commandOverride->directory;
    }

    /// @param exe What to run, and where, and under which environment.
    /// @param sandbox Whether the program runs on the sandbox's host (flatpak-spawn) or inside it.
    /// @param placement Moves each spawned child into its own resource domain before it runs; never
    ///                  null. @see ProcessPlacement.
    /// @param hangupGrace How long close() gives the child to end on the hangup before killing it.
    Process(ExecInfo exe,
            std::unique_ptr<Pty> pty,
            SandboxEscape sandbox,
            std::shared_ptr<ProcessPlacement> placement,
            HangupGrace hangupGrace = {});

    /// @param removedEnvironment Variables the child must not inherit.
    /// @param sandbox Whether the program runs on the sandbox's host (flatpak-spawn) or inside it.
    /// @param placement Moves each spawned child into its own resource domain before it runs; never
    ///                  null. @see ProcessPlacement.
    /// @param hangupGrace How long close() gives the child to end on the hangup before killing it.
    Process(std::string const& path,
            std::vector<std::string> const& args,
            std::filesystem::path const& cwd,
            Environment const& env,
            std::vector<std::string> removedEnvironment,
            SandboxEscape sandbox,
            std::unique_ptr<Pty> pty,
            std::shared_ptr<ProcessPlacement> placement,
            HangupGrace hangupGrace = {}):
        Process(ExecInfo { .program = path,
                           .arguments = args,
                           .workingDirectory = cwd,
                           .env = env,
                           .removedEnvironment = std::move(removedEnvironment) },
                std::move(pty),
                sandbox,
                std::move(placement),
                hangupGrace)
    {
    }

    ~Process() override;

    /// Takes over a running child a previous image left behind, or on Windows the daemon a successor takes
    /// over from: no spawn, no placement (it was placed when spawned). It is started already, and exit
    /// detection is as for a spawned child.
    /// @param child The child, from releaseChild() or a handover image.
    /// @param pty The PTY it runs on, usually from adoptPty().
    /// @return The process; or AdoptError::NoSuchChild where @p child is no child this process can wait for,
    ///         @p pty then destroyed with the call (@see canAdopt(), which asks first).
    [[nodiscard]] static std::expected<std::unique_ptr<Process>, AdoptError> adopt(AdoptedChild const& child,
                                                                                   std::unique_ptr<Pty> pty);

    /// Asks whether adopt() would take @p child, without taking anything: for an adopter that keeps the PTY
    /// whichever way it goes.
    /// @param child The child, from releaseChild() or a handover image.
    /// @return Nothing; or AdoptError::NoSuchChild where @p child is no child this process can wait for: on
    ///         POSIX its pid names no child of this process (dead and reaped, or foreign), on Windows its
    ///         process handle is not open here or names another process. A child that exited but is not
    ///         reaped yet is still one.
    [[nodiscard]] static std::expected<void, AdoptError> canAdopt(AdoptedChild const& child);

    /// The child as releaseChild() gives it up, while this Process keeps it: for a capture that may yet roll
    /// back, which must not have to undo anything.
    /// @return The child, or nullopt where there is none: on POSIX none was spawned, or it was reaped; on
    ///         Windows none was spawned. One that exited is still handed over, for its adopter to see exit.
    [[nodiscard]] std::optional<AdoptedChild> handoverChild() const;

    /// Gives up the child without signalling or reaping it, leaving this Process without one, so that
    /// destroying it neither kills nor waits for it; for an adopter in the same process. On Windows the
    /// handle to the child is the caller's from then on (AdoptedChild::processHandle), to adopt or close.
    /// @return The child, or nullopt where there is none running.
    [[nodiscard]] std::optional<AdoptedChild> releaseChild();

    /// @return CrossingKind::Adoptable with handoverChild()'s value: no child in it where there is none to
    ///         give, which a handover cannot carry.
    [[nodiscard]] ChildCrossing crossing() const override
    {
        return ChildCrossing { .kind = CrossingKind::Adoptable,
                               .child = handoverChild(),
                               .keeperSession = 0 };
    }

    // Tests if the current process is running inside flatpak.
    static bool isFlatpak();

    /// Where a spawn from this process runs its program, and so where the program's name is resolved:
    /// programHostFor() for the sandbox this process runs in. The one rule start() spawns by, for any
    /// caller that must answer the same question beforehand.
    /// @param escape The profile's `escape_sandbox`.
    /// @return The host the program runs on.
    [[nodiscard]] static ProgramHost programHost(SandboxEscape escape);

    /// Whether a spawn from this process can start its program in a directory held open
    /// (ExecInfo::directory): only on this machine's spawn trampoline -- never through flatpak-spawn to the
    /// host, nor on Windows.
    /// @param escape The profile's `escape_sandbox`.
    /// @return Whether such a spawn can succeed here.
    [[nodiscard]] static HeldDirectorySpawn heldDirectorySpawn(SandboxEscape escape);

    /// Whether a spawn of @p exec on this machine would find its program: looked up as start() looks it up,
    /// on the PATH the child is given -- @p exec's own, else the inherited one unless @p exec withholds it.
    /// Asked before a spawn whose failure would otherwise say only that it failed.
    /// @param exec What would be spawned.
    /// @return Whether its program is found.
    [[nodiscard]] static bool findsProgram(ExecInfo const& exec);

    // A Process has a child only once start() has spawned one. Before that, and for good when start()
    // failed before spawning, there is nothing to ask about, wait for or signal, and the calls below
    // say so rather than reaching for whatever process an unset id would name.

    /// @return Whether the child is running: false before start() spawned one, and after it exited.
    [[nodiscard]] bool alive() const noexcept;

    /// Whether the child has yet to set its terminal up. On POSIX the child -- still the spawn trampoline --
    /// flushes its terminal's queues as it takes the terminal on, before it execs the program -- and a
    /// placement can park it before that for as long as placing it takes -- so input written to the PTY
    /// before then is lost, and a capture of the session taken then cannot promise to carry it. Settles for
    /// good once observed settled.
    /// @return InProgress from the trampoline's spawn until it signalled settling (one byte), right after
    ///         the flush and the change into the working directory (or as it exits); Settled before start(),
    ///         for an adopted child, and on Windows, where the console is the child's from CreateProcess()
    ///         on.
    [[nodiscard]] SpawnProgress spawnProgress() const override;

    /// @return The child's exit status once it has exited; nullopt while it runs, or when there is none.
    [[nodiscard]] std::optional<ExitStatus> checkStatus() const override;

    /// Waits for the child to exit.
    /// @return Its exit status, or nullopt when start() never spawned a child to wait for.
    [[nodiscard]] std::optional<ExitStatus> wait();

    /// @return The directory the child is in: the one it was started in while its spawn is in progress (it
    ///         has not entered it yet, and is still in this process's), else as the system tells it;
    ///         "." where there is no child, or the system cannot tell.
    [[nodiscard]] std::string workingDirectory() const override;

    /// The name of the program leading the terminal's foreground process group: the shell at its prompt,
    /// the program it runs otherwise. Linux and macOS only.
    /// @return The name, as the operating system shortens it; none without a child, on another platform, or
    ///         on a Pty that is not a UnixPty.
    [[nodiscard]] std::optional<std::string> foregroundProcessName() const override;

    /// Asks the child to end, or does nothing when there is no child running. On Windows, which has no
    /// signals, every hint ends it outright: a held-directory child's whole job, which is its tree.
    /// @param terminationHint How to ask.
    void terminate(TerminationHint terminationHint) override;

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
    [[nodiscard]] PasteLineEndings pasteLineEndings() const noexcept override { return pty().pasteLineEndings(); }
    void interruptWrites() override { pty().interruptWrites(); }
    void allowWrites() override { pty().allowWrites(); }
    [[nodiscard]] std::expected<InheritedPty, AdoptError> handover() const override { return pty().handover(); }
    /// Gives up the PTY's handover (Pty::releaseHandover()); the child stays this Process's, to give up with
    /// releaseChild() -- first, on Windows, whose exit watcher closes the pseudoconsole.
    [[nodiscard]] std::expected<InheritedPty, AdoptError> releaseHandover() override { return pty().releaseHandover(); }
    [[nodiscard]] std::optional<SpawnDescriptors> spawnDescriptors() const noexcept override { return pty().spawnDescriptors(); }
    void closeSpawnDescriptors() override { pty().closeSpawnDescriptors(); }
    [[nodiscard]] PageSize pageSize() const noexcept override { return pty().pageSize(); }
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override { pty().resizeScreen(cells, pixels); }
    [[nodiscard]] InputEcho inputEcho() const noexcept override { return pty().inputEcho(); }
    // clang-format on

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};

} // namespace vtpty
