/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>
#include <crispy/overloaded.h>
#include <crispy/stdfs.h>

#include <fmt/format.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace terminal
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
        FileSystem::path workingDirectory;
        Environment env;
        bool escapeSandbox = true;
    };

    //! Returns login shell of current user.
    static std::vector<std::string> loginShell(bool escapeSandbox);

    static FileSystem::path homeDirectory();

    Process(ExecInfo const& exe, std::unique_ptr<Pty> pty):
        Process(exe.program, exe.arguments, exe.workingDirectory, exe.env, exe.escapeSandbox, std::move(pty))
    {
    }

    Process(const std::string& path,
            std::vector<std::string> const& args,
            FileSystem::path const& cwd,
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
    [[nodiscard]] bool isClosed() const noexcept override { return pty().isClosed(); }
    [[nodiscard]] ReadResult read(crispy::buffer_object<char>& storage, std::chrono::milliseconds timeout, size_t n) override { return pty().read(storage, timeout, n); }
    void wakeupReader() override { return pty().wakeupReader(); }
    [[nodiscard]] int write(std::string_view data) override { return pty().write(data); }
    [[nodiscard]] PageSize pageSize() const noexcept override { return pty().pageSize(); }
    void resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels = std::nullopt) override { pty().resizeScreen(cells, pixels); }
    // clang-format on

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};

} // namespace terminal

template <>
struct fmt::formatter<terminal::Process::ExitStatus>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(terminal::Process::ExitStatus const& status, format_context& ctx)
        -> format_context::iterator
    {
        return std::visit(overloaded { [&](terminal::Process::NormalExit exit) {
                                          return fmt::format_to(ctx.out(), "{} (normal exit)", exit.exitCode);
                                      },
                                       [&](terminal::Process::SignalExit exit) {
                                           char buf[256];
#if defined(_WIN32)
                                           strerror_s(buf, sizeof(buf), errno);
                                           return fmt::format_to(
                                               ctx.out(), "{} (signal number {})", buf, exit.signum);
#else
                                           return fmt::format_to(ctx.out(),
                                                                 "{} (signal number {})",
                                                                 strerror_r(errno, buf, sizeof(buf)),
                                                                 exit.signum);
#endif
                                       } },
                          status);
    }
};
