#include <crispy/StackTrace.h>
// #include <xzero/Tokenizer.h>
// #include <xzero/Buffer.h>
// #include <xzero/sysconfig.h>

#include <fmt/format.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <typeinfo>

#if !defined(_WIN32)
    #include <fcntl.h>
    #include <unistd.h>

    #include <sys/types.h>
    #include <sys/wait.h>
#endif

#if defined(HAVE_CXXABI_H)
    #include <cxxabi.h>
#endif

#if defined(HAVE_EXECINFO_H)
    #include <execinfo.h>
#endif

#if defined(HAVE_DLFCN_H)
    #include <dlfcn.h>
#endif

#if defined(HAVE_UNWIND_H)
    #include <unwind.h>
#endif

#include <iostream>

using std::nullopt;
using std::optional;
using std::regex;
using std::string;
using std::vector;

namespace crispy
{

constexpr size_t MAX_FRAMES { 128 };
constexpr size_t SKIP_FRAMES { 0 };

#if defined(__linux__) || defined(__APPLE__)
struct Pipe // {{{
{
    int pfd[2];

    Pipe(): pfd { -1, -1 }
    {
        if (pipe(pfd))
            perror("pipe");
    }

    bool good() const noexcept { return pfd[0] != -1 && pfd[1] != -1; }

    int reader() noexcept { return pfd[0]; }
    int writer() noexcept { return pfd[1]; }

    ~Pipe() { close(); }

    void close()
    {
        if (pfd[0] != -1)
            ::close(pfd[0]);
        if (pfd[1] != -1)
            ::close(pfd[1]);
    }
};
// }}}
#endif

vector<void*> StackTrace::getFrames(size_t _skip, size_t _max)
{
    vector<void*> frames;

#if defined(HAVE_BACKTRACE)
    frames.resize(_skip + _max);
    frames.resize((size_t)::backtrace(&frames[0], static_cast<int>(_skip + _max)));
    std::copy(std::next(frames.begin(), (int) _skip), frames.end(), frames.begin());
    frames.resize(frames.size() > _skip ? frames.size() - _skip : std::min(frames.size(), _skip));
#endif

    return frames;
}

optional<DebugInfo> StackTrace::getDebugInfoForFrame(void const* p)
{
    if (!p)
        return nullopt;

#if defined(__linux__)
    auto pipe = Pipe();
    if (!pipe.good())
        return nullopt;
    pid_t childPid = fork();
    switch (childPid)
    {
        case -1: perror("vfork"); return nullopt;
        case 0: // in child
        {
            std::string addr2lineExe = "/usr/bin/addr2line";
            char exe[512] {};
            if (ssize_t rv = readlink("/proc/self/exe", exe, sizeof(exe)); rv < 0)
                _exit(EXIT_FAILURE);
            char addr[32];
            snprintf(addr, sizeof(addr), "%p", p);
            char const* const argv[] = { addr2lineExe.c_str(), "-pe", exe, addr, nullptr };
            close(STDIN_FILENO);
            if (pipe.writer() != STDOUT_FILENO)
            {
                dup2(pipe.writer(), STDOUT_FILENO);
                close(pipe.writer());
            }
            close(pipe.reader());
            close(STDERR_FILENO);
            auto const rv = execv(argv[0], (char**) argv);
            perror("execvp");
            _exit(rv);
        }
        default: // in parent
        {
            int status = 0;
            waitpid(childPid, &status, 0);
            // if (!WIFEXITED(status))
            //     return nullopt;
            // if (WEXITSTATUS(status) != EXIT_SUCCESS)
            //     return nullopt;

            char buf[4096];
            ssize_t len = 0;
            len = ::read(pipe.reader(), buf, sizeof(buf));
            while (len > 0 && std::isspace(buf[len - 1]))
                --len;

            // result on success:
            DebugInfo info;
    #if 0
            auto static const re = regex(R"(^(.*):\d+$)");
            std::cmatch cm;
            if (!std::regex_search((char const*)buf, (char const*)buf + len, cm, re))
                return nullopt;

            info.text = cm[1].str();
            try { info.text = stoi(cm[2].str()); } catch (...) {}
    #else
            if (len > 0)
                info.text = string(buf, (size_t) len);
    #endif

            if (info.text == "??:0")
                return nullopt;

            return { std::move(info) };
        }
    }
#else
    return nullopt;
#endif
}

StackTrace::StackTrace():
#if defined(HAVE_BACKTRACE)
    frames_
{
    SKIP_FRAMES + MAX_FRAMES
}
#else
    frames_
{
}
#endif
{
#if defined(HAVE_BACKTRACE)
    frames_.resize((size_t)::backtrace(&frames_[0], SKIP_FRAMES + MAX_FRAMES));
#endif
}

string StackTrace::demangleSymbol(const char* symbol)
{
#if defined(__linux__) || defined(__APPLE__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(symbol, nullptr, 0, &status);

    if (demangled)
    {
        string result(demangled, strlen(demangled));
        free(demangled);
        return result;
    }
    else
    {
        return symbol;
    }
#else
    return symbol;
#endif
}

vector<string> StackTrace::symbols() const
{
    if (empty())
        return {};

    auto const frames = getFrames(SKIP_FRAMES, MAX_FRAMES);
    vector<string> output;
    for (auto const* frame: frames)
    {
#if defined(CONTOUR_STACKTRACE_ADDR2LINE)
        auto debugInfo = getDebugInfoForFrame(frame);
        if (!debugInfo)
            output.emplace_back(fmt::format("{}", frame));
        else
            output.emplace_back(debugInfo->text);
#else
        output.emplace_back(fmt::format("{}", frame));
#endif
    }
    return output;
}

} // namespace crispy
