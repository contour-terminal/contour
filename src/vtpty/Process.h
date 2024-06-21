// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>
#include <crispy/overloaded.h>

#include <fmt/format.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace vtpty
{

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
        bool escapeSandbox = true;
    };

    //! Returns login shell of current user.
    static std::vector<std::string> loginShell(bool escapeSandbox);

    static std::string userName();
    static std::filesystem::path homeDirectory();

    Process(ExecInfo const& exe, std::unique_ptr<Pty> pty):
        Process(exe.program, exe.arguments, exe.workingDirectory, exe.env, exe.escapeSandbox, std::move(pty))
    {
    }

    Process(const std::string& path,
            std::vector<std::string> const& args,
            std::filesystem::path const& cwd,
            Environment const& env,
            bool escapeSandbox,
            std::unique_ptr<Pty> pty);

    ~Process() override;

    // Tests if the current process is running inside flatpak.
    static bool isFlatpak();

    [[nodiscard]] bool alive() const noexcept
    {
        auto const status = checkStatus();
        return !status.has_value()
               || !(std::holds_alternative<NormalExit>(*status)
                    || std::holds_alternative<SignalExit>(*status));
    }

    [[nodiscard]] std::optional<ExitStatus> checkStatus() const;
    [[nodiscard]] ExitStatus wait();

    [[nodiscard]] std::string workingDirectory() const;

    enum class TerminationHint
    {
        Normal,
        Hangup
    };
    void terminate(TerminationHint terminationHint);

    [[nodiscard]] Pty& pty() noexcept;
    [[nodiscard]] Pty const& pty() const noexcept;

    // Pty overrides
    // clang-format off
    void start() override;
    [[nodiscard]] PtySlave& slave() noexcept override { return pty().slave(); }
    void close() override { pty().close(); }
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override { return pty().isClosed(); }
    [[nodiscard]] std::optional<ReadResult> read(crispy::buffer_object<char>& storage, std::optional<std::chrono::milliseconds> timeout, size_t n) override { return pty().read(storage, timeout, n); }
    void wakeupReader() override { return pty().wakeupReader(); }
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
struct fmt::formatter<vtpty::Process::ExitStatus>: fmt::formatter<std::string>
{
    auto format(vtpty::Process::ExitStatus const& status, format_context& ctx) -> format_context::iterator
    {
        auto const text =
            std::visit(overloaded { [&](vtpty::Process::NormalExit exit) {
                                       return fmt::format("{} (normal exit)", exit.exitCode);
                                   },
                                    [&](vtpty::Process::SignalExit exit) {
                                        char buf[256];
#if defined(_WIN32)
                                        strerror_s(buf, sizeof(buf), errno);
                                        return fmt::format("{} (signal number {})", buf, exit.signum);
#else
                                        return fmt::format("{} (signal number {})",
                                                           strerror_r(errno, buf, sizeof(buf)),
                                                           exit.signum);
#endif
                                    } },
                       status);
        return fmt::formatter<std::string>::format(text, ctx);
    }
};
