// SPDX-License-Identifier: Apache-2.0

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__OpenBSD__)
    #include <pthread_np.h>
    #define pthread_getname_np pthread_get_name_np
#else
    #include <pthread.h>
#endif

#include <crispy/utils.h>

namespace crispy
{

using namespace std::string_literals;

std::string threadName()
{
#if defined(_WIN32)
    auto const ThreadHandle = GetCurrentThread();
    PWSTR pwsz = nullptr;
    HRESULT hr = GetThreadDescription(ThreadHandle, &pwsz);
    if (SUCCEEDED(hr))
    {
        int const len = WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8Str(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, utf8Str.data(), len, nullptr, nullptr);
        utf8Str.resize(len - 1);
        LocalFree(pwsz);
        return utf8Str;
    }
    return ""s;
#else
    char text[32] = {};
    pthread_getname_np(pthread_self(), text, sizeof(text));
    return text;
#endif
}

} // namespace crispy
