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

#include <terminal/pty/Pty.h>

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
    };

    //! Returns login shell of current user.
    static std::vector<std::string> loginShell();

    static FileSystem::path homeDirectory();

    Process(ExecInfo const& _exe, std::unique_ptr<Pty> _pty):
        Process(_exe.program, _exe.arguments, _exe.workingDirectory, _exe.env, std::move(_pty))
    {
    }

    Process(const std::string& path,
            std::vector<std::string> const& args,
            FileSystem::path const& _cwd,
            Environment const& env,
            std::unique_ptr<Pty> pty);

    ~Process() override;

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
    void terminate(TerminationHint _terminationHint);

    Pty& pty() noexcept;
    Pty const& pty() const noexcept;

    // Pty overrides
    // clang-format off
    PtySlave& slave() noexcept override { return pty().slave(); }
    void close() override { pty().close(); }
    bool isClosed() const noexcept override { return pty().isClosed(); }
    std::optional<std::string_view> read(size_t _size, std::chrono::milliseconds _timeout) override { return pty().read(_size, _timeout); }
    [[nodiscard]] std::optional<std::tuple<std::string_view, bool>> read(crispy::BufferObject& storage, std::chrono::milliseconds timeout, size_t n) override { return pty().read(storage, timeout, n); }
    void wakeupReader() override { return pty().wakeupReader(); }
    int write(char const* buf, size_t size) override { return pty().write(buf, size); }
    PageSize pageSize() const noexcept override { return pty().pageSize(); }
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt) override { pty().resizeScreen(_cells, _pixels); }
    // clang-format on

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> d;
};

} // namespace terminal

namespace fmt
{
template <>
struct formatter<terminal::Process::ExitStatus>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::Process::ExitStatus const& _status, FormatContext& _ctx)
    {
        return std::visit(overloaded { [&](terminal::Process::NormalExit _exit) {
                                          return format_to(_ctx.out(), "{} (normal exit)", _exit.exitCode);
                                      },
                                       [&](terminal::Process::SignalExit _exit) {
                                           char buf[256];
#if defined(_WIN32)
                                           strerror_s(buf, sizeof(buf), errno);
#else
                                           strerror_r(errno, buf, sizeof(buf));
#endif
                                           return format_to(
                                               _ctx.out(), "{} (signal number {})", buf, _exit.signum);
                                       } },
                          _status);
    }
};
} // namespace fmt
