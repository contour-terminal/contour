#include <terminal/Process.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <pty.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

namespace terminal {

WindowSize currentWindowSize()
{
    auto w = winsize{};

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    return WindowSize{w.ws_row, w.ws_col};
}

auto const envvars =
    unordered_map<string, string>{{"TERM", "xterm-256color"}, {"COLORTERM", "xterm"}, {"COLORFGBG", "15;0"},
                                  {"LINES", ""},              {"COLUMNS", ""},        {"TERMCAP", ""}};

std::string Process::loginShell()
{
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
        return pw->pw_shell;
    else
        return "/bin/sh";
}

static inline pid_t forkpty(int* fd, winsize const& ws)
{
    return ::forkpty(fd, nullptr, nullptr, &ws);
}

Process::Process(WindowSize const& windowSize, const string& path)
    : fd_{-1}, pid_{forkpty(&fd_, winsize{windowSize.rows, windowSize.columns})}
{
    if (pid_ == 0)
    {
        // in child process
        for (auto&& [name, value] : envvars)
            setenv(name.c_str(), value.c_str(), true);

        ::execl(path.c_str(), path.c_str(), nullptr);
        ::_exit(EXIT_FAILURE);
    }
    else if (pid_ < 0)
        throw std::runtime_error(strerror(errno));
}

Process::ExitStatus Process::wait()
{
    int status = 0;
    waitpid(pid_, &status, 0);

    if (WIFEXITED(status))
        return NormalExit{WEXITSTATUS(status)};

    if (WIFSIGNALED(status))
        return SignalExit{WTERMSIG(status)};

    if (WIFSTOPPED(status))
        return Suspend{};

    if (WIFCONTINUED(status))
        return Resume{};

    throw runtime_error{"Unknown waitpid() return value."};
}

ssize_t Process::send(void const* data, size_t size)
{
    return ::write(fd_, data, size);
}

int Process::receive(uint8_t* data, size_t size)
{
    return ::read(fd_, data, size);
}

}  // namespace terminal
