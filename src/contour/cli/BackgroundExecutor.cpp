// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/BackgroundExecutor.hpp>

#ifndef _WIN32
    #include <csignal>

    #include <pthread.h>
#endif

namespace contour::cli
{

namespace
{
#ifndef _WIN32
    /// Blocks every signal in the constructing thread and restores the previous mask on destruction.
    ///
    /// A thread inherits the signal mask of the thread that creates it, so threads started while this
    /// guard is alive never receive an asynchronous signal.
    class ScopedAllSignalsBlocked
    {
      public:
        ScopedAllSignalsBlocked() noexcept
        {
            auto all = sigset_t {};
            sigfillset(&all);
            // Synchronous fault signals stay deliverable: they are raised by the faulting thread itself,
            // and blocking them turns a crash into a silent kill with no sanitizer or crash report.
            for (auto const fault: { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP })
                sigdelset(&all, fault);
            pthread_sigmask(SIG_BLOCK, &all, &_previous);
        }
        ScopedAllSignalsBlocked(ScopedAllSignalsBlocked const&) = delete;
        ScopedAllSignalsBlocked& operator=(ScopedAllSignalsBlocked const&) = delete;
        ScopedAllSignalsBlocked(ScopedAllSignalsBlocked&&) = delete;
        ScopedAllSignalsBlocked& operator=(ScopedAllSignalsBlocked&&) = delete;
        ~ScopedAllSignalsBlocked() { pthread_sigmask(SIG_SETMASK, &_previous, nullptr); }

      private:
        sigset_t _previous {};
    };
#endif
} // namespace

core::async::ThreadPoolExecutor makeBackgroundExecutor(std::size_t threads)
{
#ifndef _WIN32
    auto const signalsBlocked = ScopedAllSignalsBlocked {};
#endif
    return core::async::ThreadPoolExecutor { threads };
}

} // namespace contour::cli
