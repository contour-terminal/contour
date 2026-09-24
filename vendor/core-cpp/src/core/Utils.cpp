// SPDX-License-Identifier: Apache-2.0

#ifdef _WIN32
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

    // One exit past this point, so the description is always freed. A conversion that fails
    // answers 0, and `len - 1` as a std::size_t underflowed: resize() threw length_error, and
    // the LocalFree() below it never ran.
    auto result = ""s;
    if (int const len = WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, nullptr, 0, nullptr, nullptr); len > 0)
    {
        result.resize(static_cast<std::size_t>(len));
        WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, result.data(), len, nullptr, nullptr);
        result.resize(static_cast<std::size_t>(len - 1)); // drop the NUL the conversion wrote
    }
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
