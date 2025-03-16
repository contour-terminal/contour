#include <crispy/StackTrace.h>
#include <crispy/utils.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <regex>

#if !defined(_WIN32)
    #include <sys/types.h>
    #include <sys/wait.h>

    #include <fcntl.h>
    #include <unistd.h>
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

using std::nullopt;
using std::optional;
using std::regex;
using std::string;
using std::vector;

namespace crispy
{

constexpr size_t MAX_FRAMES { 128 }; // NOLINT
constexpr size_t SKIP_FRAMES { 0 };  // NOLINT

#if defined(__linux__) || defined(__APPLE__)
struct system_wrap // {{{
{
    int pfd[2];

    system_wrap(): pfd { -1, -1 }
    {
        if (pipe(pfd))
            perror("pipe");
    }

    [[nodiscard]] bool good() const noexcept { return pfd[0] != -1 && pfd[1] != -1; }
    [[nodiscard]] int reader() noexcept { return pfd[0]; }
    [[nodiscard]] int writer() noexcept { return pfd[1]; }

    ~system_wrap() { close(); }

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

vector<void*> stack_trace::getFrames(size_t skip, size_t max)
{
    vector<void*> frames;

#if defined(HAVE_BACKTRACE)
    frames.resize(skip + max);
    frames.resize((size_t) ::backtrace(frames.data(), static_cast<int>(skip + max)));
    std::copy(std::next(frames.begin(), (int) skip), frames.end(), frames.begin());
    frames.resize(frames.size() > skip ? frames.size() - skip : std::min(frames.size(), skip));
#else
    crispy::ignore_unused(skip, max);
#endif

    return frames;
}

optional<debug_info> stack_trace::getDebugInfoForFrame(void const* frameAddress)
{
    if (!frameAddress)
        return nullopt;

#if defined(__linux__)
    auto pipe = system_wrap();
    if (!pipe.good())
        return nullopt;
    pid_t const childPid = fork();
    switch (childPid)
    {
        case -1: perror("vfork"); return nullopt;
        case 0: // in child
        {
            std::string const addr2lineExe = "/usr/bin/addr2line";
            char exe[512] {};
            if (ssize_t const rv = readlink("/proc/self/exe", exe, sizeof(exe)); rv < 0)
                _exit(EXIT_FAILURE);
            char addr[32];
            snprintf(addr, sizeof(addr), "%p", frameAddress);
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
            debug_info info;
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

stack_trace::stack_trace():
#if defined(HAVE_BACKTRACE)
    _frames { SKIP_FRAMES + MAX_FRAMES }
#else
    _frames {}
#endif
{
#if defined(HAVE_BACKTRACE)
    _frames.resize((size_t) ::backtrace(_frames.data(), SKIP_FRAMES + MAX_FRAMES));
#endif
}

string stack_trace::demangleSymbol(const char* symbol)
{
#if (defined(__linux__) || defined(__APPLE__)) && defined(HAVE_CXXABI_H)
    int status = 0;
    char* demangled = abi::__cxa_demangle(symbol, nullptr, nullptr, &status);

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

vector<string> stack_trace::symbols() const
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
            output.emplace_back(std::format("{}", frame));
        else
            output.emplace_back(debugInfo->text);
#else
        output.emplace_back(std::format("{}", frame));
#endif
    }
    return output;
}

} // namespace crispy
