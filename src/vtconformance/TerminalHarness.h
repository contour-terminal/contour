// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PageSize.h>
#include <vtpty/Process.h>
#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <vtconformance/Diagnostics.h>
#include <vtconformance/MarkerScanner.h>
#include <vtconformance/ScreenDump.h>
#include <vtconformance/Suite.h>
#include <vtconformance/TerminalEngine.h>

namespace vtconformance
{

/// Drives a program under a real PTY against a `TerminalEngine`.
///
/// This is the only unit in the module that spawns a process. Everything it produces — screen dumps,
/// diagnostics, the program's own log — is handed to pure functions for judgement.
class TerminalHarness
{
  public:
    /// How much `readBatch` reads at once. Any size does: the parser is byte-sequential, so where a
    /// read happens to end never changes the screen. @see readBatch.
    static constexpr size_t ReadBufferSize = 16384;

    struct Options
    {
        /// Terminal geometry and identity. @see TerminalEngine::Options.
        TerminalEngine::Options engine {};

        /// How long to wait for the child to react at all before declaring it wedged.
        std::chrono::milliseconds stepTimeout { 10000 };
    };

    /// @param exec    The program to spawn.
    /// @param options Terminal geometry and timing.
    TerminalHarness(vtpty::Process::ExecInfo const& exec, Options options);
    ~TerminalHarness();

    TerminalHarness(TerminalHarness const&) = delete;
    TerminalHarness& operator=(TerminalHarness const&) = delete;
    TerminalHarness(TerminalHarness&&) = delete;
    TerminalHarness& operator=(TerminalHarness&&) = delete;

    /// One read from the child, and the markers that finished inside it.
    struct Batch
    {
        /// The bytes read. Borrowed: valid until the next `readBatch`, and not yet fed to the terminal.
        std::string_view bytes;

        /// The markers that finished in `bytes`, in stream order. @see MarkerScanner.
        std::vector<MarkerScanner::Match> matches;

        /// Whether the child's output has ended. `bytes` may still hold its last words.
        bool closed = false;
    };

    /// Who reads the child's output.
    enum class Pumping : std::uint8_t
    {
        /// A background thread reads and feeds, indivisibly. The caller never sees the bytes.
        Internally,

        /// The caller drives the reads with `readBatch` and decides where the stream is cut.
        ///
        /// There is exactly one reader either way: a pump thread racing `readBatch` for the same device
        /// would split the byte stream between them at random.
        ByTheCaller,
    };

    /// Spawns the child and, unless the caller wants the reads, starts pumping its output.
    void start(Pumping pumping = Pumping::Internally);

    /// Reads one batch from the child and scans it, WITHOUT feeding it to the terminal.
    ///
    /// Feeding is the caller's, because the caller is the only one that knows what a marker means: the
    /// screen a banner announces is the bytes up to it and not one byte more, so whoever wants to
    /// capture that screen has to be the one who decides where to stop feeding. @see feed.
    ///
    /// Only meaningful once `startScanning` has been called.
    [[nodiscard]] Batch readBatch();

    /// Feeds @p bytes to the terminal. @see readBatch.
    void feed(std::string_view bytes) { _engine->writeToScreen(bytes); }

    /// Starts scanning the child's output for @p markers. @see readBatch.
    void startScanning(std::span<std::string_view const> markers);

    /// Stops the pump thread and terminates the child if it is still alive.
    void stop();

    /// @return Whether the child process is still running.
    [[nodiscard]] bool alive() const noexcept;

    /// Waits for a self-driving program to run to completion.
    ///
    /// The counterpart to scanning for a prompt marker (@see startScanning, readBatch), for suites that
    /// need no driving at all: esctest walks its own test list and exits, so the only barrier is the
    /// child's death.
    ///
    /// @return false if the child was still running when @p timeout elapsed.
    [[nodiscard]] bool waitForExit(std::chrono::milliseconds timeout) const;

    /// @return The visible page as plain text, read under the terminal lock.
    [[nodiscard]] std::string screenText() const { return _engine->screenText(); }

    /// Writes @p text to the child's stdin.
    void writeInput(std::string_view text);

    /// @return A golden-format dump of the current screen, read under the terminal lock.
    [[nodiscard]] std::string dump(DumpOptions const& options = {}) const { return _engine->dump(options); }

    /// @return The engine diagnostics collected so far (oracle A).
    [[nodiscard]] std::vector<Diagnostic> diagnostics() const { return _engine->diagnostics(); }

    /// The terminal the child is being driven against.
    [[nodiscard]] TerminalEngine& engine() noexcept { return *_engine; }

  private:
    /// Feeds the child's output into the terminal until it closes.
    void pump();

    Options _options;

    // Declared before `_engine`: the device is built as its initializer runs, and reports this back
    // on the way past. @see buildDevice.

    /// Borrowed. Owned by the PTY the engine was handed as its device.
    vtpty::Process* _process = nullptr;

    std::unique_ptr<TerminalEngine> _engine;
    std::atomic<bool> _terminating { false };
    std::unique_ptr<std::thread> _pumpThread;

    /// Set only while a caller drives the reads itself. @see startScanning, readBatch.
    std::optional<MarkerScanner> _scanner;

    /// The buffer `readBatch` reads into. `Pty::read` requires a pooled buffer object.
    crispy::buffer_object_pool<char> _bufferPool { ReadBufferSize };
    crispy::buffer_object_ptr<char> _readBuffer;
};

} // namespace vtconformance
