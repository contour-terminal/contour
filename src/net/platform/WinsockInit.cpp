// SPDX-License-Identifier: Apache-2.0
#include <net/platform/WinsockInit.h>

#ifdef _WIN32
// clang-format off
    #include <winsock2.h>
    #include <windows.h>
// clang-format on
#endif

namespace net
{

#ifdef _WIN32

namespace
{
    /// RAII guard performing WSAStartup on construction and WSACleanup at process
    /// exit. A single function-local static instance gives idempotent, thread-safe
    /// (C++11 static-init) one-time initialization.
    struct WinsockGuard
    {
        WinsockGuard() noexcept
        {
            WSADATA data {};
            WSAStartup(MAKEWORD(2, 2), &data);
        }

        ~WinsockGuard() { WSACleanup(); }

        WinsockGuard(WinsockGuard const&) = delete;
        WinsockGuard& operator=(WinsockGuard const&) = delete;
        WinsockGuard(WinsockGuard&&) = delete;
        WinsockGuard& operator=(WinsockGuard&&) = delete;
    };
} // namespace

void ensureWinsockInitialized() noexcept
{
    static WinsockGuard const guard;
    static_cast<void>(guard);
}

#else

void ensureWinsockInitialized() noexcept
{
}

#endif

} // namespace net
