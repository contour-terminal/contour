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

#include <crispy/overloaded.h>

#include <fmt/format.h>

#include <crispy/stdfs.h>

#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <mutex>
#include <variant>
#include <vector>

#if defined(_MSC_VER)
#include <Windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <terminal/pty/UnixPty.h>
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

namespace terminal {

class Pty;

/**
 * Spawns and manages a child process with a pseudo terminal attached to it.
 */
class [[nodiscard]] Process {
  public:
#if defined(__unix__) || defined(__APPLE__)
	using NativeHandle = pid_t;
#else
	using NativeHandle = HANDLE;
#endif

    struct NormalExit { int exitCode; };
    struct SignalExit { int signum; };

    using ExitStatus = std::variant<NormalExit, SignalExit>;
	using Environment = std::map<std::string, std::string>;

    struct ExecInfo {
        std::string program;
        std::vector<std::string> arguments;
        FileSystem::path workingDirectory;
        Environment env;
    };

    //! Returns login shell of current user.
    static std::vector<std::string> loginShell();

    static FileSystem::path homeDirectory();

    Process(ExecInfo const& _exe, Pty& _pty) :
        Process(_exe.program, _exe.arguments, _exe.workingDirectory, _exe.env, _pty)
    {
    }

    Process(
		const std::string& path,
		std::vector<std::string> const& args,
        FileSystem::path const& _cwd,
		Environment const& env,
		Pty& pty
	);

    Process(
		const std::string& _path,
		std::vector<std::string> const& _args,
        FileSystem::path const& _cwd,
		Environment const& _env,
		bool _detached
	);

	~Process();

	[[nodiscard]] NativeHandle nativeHandle() const noexcept { return pid_; }
    [[nodiscard]] bool alive() const noexcept;

    [[nodiscard]] std::optional<ExitStatus> checkStatus() const;
	[[nodiscard]] ExitStatus wait();

    [[nodiscard]] std::string workingDirectory(Pty const* _pty = nullptr) const;

    enum class TerminationHint {
        Normal,
        Hangup
    };
    void terminate(TerminationHint _terminationHint);

private:
    [[nodiscard]] std::optional<ExitStatus> checkStatus(bool _waitForExit) const;
	mutable NativeHandle pid_{};
	bool detached_ = false;
    mutable std::mutex lock_;

#if defined(_MSC_VER)
	PROCESS_INFORMATION processInfo_{};
	STARTUPINFOEX startupInfo_{};
#endif

    mutable std::optional<Process::ExitStatus> exitStatus_{};
};

}  // namespace terminal

namespace fmt
{
    template <>
    struct formatter<terminal::Process::ExitStatus> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Process::ExitStatus const& _status, FormatContext& _ctx)
        {
            return std::visit(overloaded{
                [&](terminal::Process::NormalExit _exit) {
                    return format_to(_ctx.out(), "{} (normal exit)", _exit.exitCode);
                },
                [&](terminal::Process::SignalExit _exit) {
                    char buf[256];
                    #if defined(_WIN32)
                    strerror_s(buf, sizeof(buf), errno);
                    #else
                    strerror_r(errno, buf, sizeof(buf));
                    #endif
                    return format_to(_ctx.out(), "{} (signal number {})", buf, _exit.signum);
                }
            }, _status);
        }
    };
}
