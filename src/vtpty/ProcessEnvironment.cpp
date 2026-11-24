// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessEnvironment.hpp>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <cwchar>
    #include <string_view>

    #include <Windows.h>
#else
    #include <vtpty/UnixUtils.hpp>
#endif

namespace vtpty
{

std::vector<std::string> processEnvironmentEntries()
{
    auto entries = std::vector<std::string> {};
#ifdef _WIN32
    auto* const block = GetEnvironmentStringsW();
    if (block == nullptr)
        return entries;
    for (auto const* entry = block; *entry != L'\0';)
    {
        auto const text = std::wstring_view { entry, std::wcslen(entry) };
        if (!text.starts_with(L'='))
            entries.push_back(core::toUtf8(text));
        entry += text.size() + 1;
    }
    FreeEnvironmentStringsW(block);
#else
    for (auto const entry: util::environmentEntries())
        entries.emplace_back(entry);
#endif
    return entries;
}

} // namespace vtpty
