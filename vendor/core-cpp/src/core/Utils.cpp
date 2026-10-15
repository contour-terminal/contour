// SPDX-License-Identifier: Apache-2.0

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <windows.h>
#elifdef __EMSCRIPTEN__
// Single-threaded WebAssembly: one thread, and no name to give it.
#elifdef __OpenBSD__
    #include <pthread_np.h>
    #define pthread_getname_np pthread_get_name_np
#else
    #include <pthread.h>
#endif

#include <core/Utils.hpp>

namespace core
{

using namespace std::string_literals;

std::string threadName()
{
#ifdef _WIN32
    auto const threadHandle = GetCurrentThread();
    PWSTR pwsz = nullptr;
    HRESULT const hr = GetThreadDescription(threadHandle, &pwsz);
    if (FAILED(hr))
        return ""s;

    // One exit past this point, so the description is always freed.
    auto result = toUtf8(pwsz);
    LocalFree(pwsz);
    return result;
#elifdef __EMSCRIPTEN__
    // Emscripten's libc leaves pthread_getname_np() out, and without pthreads there is nothing
    // to name.
    return ""s;
#else
    char text[32] = {};
    pthread_getname_np(pthread_self(), text, sizeof(text));
    return text;
#endif
}

} // namespace core
