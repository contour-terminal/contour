// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ThreadCpuClock.hpp>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <pthread.h>
#endif

#ifdef __APPLE__
    #include <array>
    #include <cstring>

    #include <mach/mach.h>
#elif !defined(_WIN32)
    #include <time.h>
#endif

namespace vtextension
{

namespace
{
#ifdef _WIN32
    /// @return The 100-nanosecond intervals @p time holds.
    [[nodiscard]] std::uint64_t intervalsOf(FILETIME const& time) noexcept
    {
        return (std::uint64_t { time.dwHighDateTime } << 32U) | time.dwLowDateTime;
    }

    /// Names a thread by its id, and opens it only for as long as one reading takes.
    class NativeThreadCpuClock final: public IThreadCpuClock
    {
      public:
        [[nodiscard]] std::optional<ThreadCpuId> currentThread() const noexcept override
        {
            return ThreadCpuId { .value = ::GetCurrentThreadId() };
        }

        [[nodiscard]] std::optional<std::chrono::nanoseconds> consumed(
            ThreadCpuId thread) const noexcept override
        {
            auto* const handle =
                ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(thread.value));
            if (handle == nullptr)
                return std::nullopt;
            auto creation = FILETIME {};
            auto exit = FILETIME {};
            auto kernel = FILETIME {};
            auto user = FILETIME {};
            auto const read = ::GetThreadTimes(handle, &creation, &exit, &kernel, &user) != 0;
            ::CloseHandle(handle);
            if (!read)
                return std::nullopt;
            using Intervals = std::chrono::duration<std::uint64_t, std::ratio<1, 10'000'000>>;
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                Intervals { intervalsOf(kernel) + intervalsOf(user) });
        }
    };
#elif defined(__APPLE__)
    /// Names a thread by its Mach port, which needs no release while the thread lives.
    class NativeThreadCpuClock final: public IThreadCpuClock
    {
      public:
        [[nodiscard]] std::optional<ThreadCpuId> currentThread() const noexcept override
        {
            return ThreadCpuId { .value = ::pthread_mach_thread_np(::pthread_self()) };
        }

        [[nodiscard]] std::optional<std::chrono::nanoseconds> consumed(
            ThreadCpuId thread) const noexcept override
        {
            // thread_info() fills an integer_t buffer; the record is copied out of it, not cast onto it.
            auto buffer = std::array<integer_t, THREAD_BASIC_INFO_COUNT> {};
            auto count = mach_msg_type_number_t { THREAD_BASIC_INFO_COUNT };
            if (::thread_info(
                    static_cast<thread_act_t>(thread.value), THREAD_BASIC_INFO, buffer.data(), &count)
                != KERN_SUCCESS)
                return std::nullopt;
            auto info = thread_basic_info_data_t {};
            std::memcpy(&info, buffer.data(), sizeof(info));
            auto const timeOf = [](time_value_t const& time) {
                return std::chrono::seconds { time.seconds }
                       + std::chrono::microseconds { time.microseconds };
            };
            return std::chrono::duration_cast<std::chrono::nanoseconds>(timeOf(info.user_time)
                                                                        + timeOf(info.system_time));
        }
    };
#else
    /// Names a thread by its POSIX CPU-time clock.
    class NativeThreadCpuClock final: public IThreadCpuClock
    {
      public:
        [[nodiscard]] std::optional<ThreadCpuId> currentThread() const noexcept override
        {
            auto clock = clockid_t {};
            if (::pthread_getcpuclockid(::pthread_self(), &clock) != 0)
                return std::nullopt;
            return ThreadCpuId { .value = static_cast<std::uint64_t>(clock) };
        }

        [[nodiscard]] std::optional<std::chrono::nanoseconds> consumed(
            ThreadCpuId thread) const noexcept override
        {
            auto time = timespec {};
            if (::clock_gettime(static_cast<clockid_t>(thread.value), &time) != 0)
                return std::nullopt;
            return std::chrono::seconds { time.tv_sec } + std::chrono::nanoseconds { time.tv_nsec };
        }
    };
#endif
} // namespace

IThreadCpuClock const& nativeThreadCpuClock() noexcept
{
    static auto const clock = NativeThreadCpuClock {};
    return clock;
}

} // namespace vtextension
