// SPDX-License-Identifier: Apache-2.0
///
/// @file
/// The canary for what a strand does with a task that throws out of `resume()`.
///
/// Two behaviours, one per compiler family, and each is a process's worth of evidence rather than
/// a Catch case's, because one of them ends the process:
///
///   - everywhere but MSVC's `cl`, the exception reaches whoever resumed the strand on its base, and
///     the task queued behind it still runs (`Strand_test.cpp` asserts the same in-process);
///   - under `cl`, the strand ends the process with a message, because an exception crossing its
///     coroutine frames was measured corrupting the thread's executor scopes there.
///
/// `src/core/async/CMakeLists.txt` registers it with the expression that is the PASS on the
/// compiler it was built by and the other one as the FAIL, so a build that took the wrong branch
/// fails by name. The markers go to `stderr`, which is unbuffered, before each step.

#include <core/async/Strand.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>

#include <coroutine>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <tuple>

namespace
{

/// Turns the terminate's abort into an exit status: ctest reads a raw signal as a failure that no
/// regular expression overrides, so without this the `cl` branch could not pass by its message.
/// @param signalNumber Ignored; only SIGABRT is handled.
extern "C" void onAbort(int signalNumber)
{
    std::ignore = signalNumber;
    std::_Exit(1);
}

/// A coroutine whose `resume()` throws: its promise rethrows what escapes the body.
class ThrowingResume
{
  public:
    struct promise_type
    {
        [[nodiscard]] ThrowingResume get_return_object() noexcept
        {
            return ThrowingResume { std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        [[noreturn]] void unhandled_exception() const { throw; }
    };

    explicit ThrowingResume(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}
    ThrowingResume(ThrowingResume const&) = delete;
    ThrowingResume(ThrowingResume&&) = delete;
    ThrowingResume& operator=(ThrowingResume const&) = delete;
    ThrowingResume& operator=(ThrowingResume&&) = delete;
    ~ThrowingResume() { _handle.destroy(); }

    [[nodiscard]] std::coroutine_handle<> handle() const noexcept { return _handle; }

  private:
    std::coroutine_handle<promise_type> _handle;
};

/// Throws out of `resume()`.
ThrowingResume throwOutOfResume(bool really)
{
    if (really)
        throw std::runtime_error { "a task that throws out of resume()" };
    co_return;
}

/// Records that it ran.
core::async::Task<void> recordRan(bool* ran)
{
    *ran = true;
    co_return;
}

} // namespace

int main()
{
    std::ignore = std::signal(SIGABRT, &onAbort);

    auto base = core::async::testing::ManualExecutor {};
    auto strand = core::async::Strand { base };
    auto ran = false;
    auto const thrower = throwOutOfResume(true);
    auto next = recordRan(&ran);
    strand.submit(thrower.handle());
    strand.submit(next.handle());

    std::fputs("strand-throw-canary: resuming a task that throws out of resume()\n", stderr);
    try
    {
        std::ignore = base.drain();
    }
    catch (std::runtime_error const&)
    {
        std::fputs("strand-throw-canary: the throw reached the base\n", stderr);
    }
    std::ignore = base.drain();
    if (ran)
        std::fputs("strand-throw-canary: the task behind it ran\n", stderr);
    return 0;
}
