// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/LaunchRefusal_win32.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace vtextension::wasm::remote::process
{

namespace
{
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, LaunchSteps.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(LaunchSteps[index].step) == index;
                                      }),
                  "LaunchSteps holds each step at its enumerator's index");
    static_assert(LaunchSteps.back().step == LaunchStep::CreateProcess, "LaunchSteps holds every step");

    /// Frees what FormatMessageW() allocated.
    struct LocalMemoryFree
    {
        void operator()(wchar_t* memory) const noexcept { ::LocalFree(memory); }
    };

    /// @return What the system calls @p code, as UTF-8, without its line break; empty if it has no words.
    [[nodiscard]] std::string systemWordsFor(DWORD code)
    {
        wchar_t* buffer = nullptr;
        auto const length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                                 | FORMAT_MESSAGE_IGNORE_INSERTS,
                                             nullptr,
                                             code,
                                             0,
                                             reinterpret_cast<wchar_t*>(&buffer),
                                             0,
                                             nullptr);
        auto const owned = std::unique_ptr<wchar_t, LocalMemoryFree> { buffer };
        if (length == 0 || buffer == nullptr)
            return {};
        auto words = std::wstring_view { buffer, length };
        while (!words.empty() && (words.back() == L'\r' || words.back() == L'\n' || words.back() == L' '))
            words.remove_suffix(1);
        if (words.empty())
            return {};
        auto const size = ::WideCharToMultiByte(
            CP_UTF8, 0, words.data(), static_cast<int>(words.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
            return {};
        auto utf8 = std::string(static_cast<std::size_t>(size), '\0');
        ::WideCharToMultiByte(
            CP_UTF8, 0, words.data(), static_cast<int>(words.size()), utf8.data(), size, nullptr, nullptr);
        return utf8;
    }
} // namespace

std::string launchRefusalLine(LaunchRefusal const& refusal)
{
    auto const& row = LaunchSteps[std::to_underlying(refusal.step)];
    auto const code = row.code == RefusalCode::HResult ? std::format("0x{:08X}", refusal.code)
                                                       : std::format("{}", refusal.code);
    auto const words = systemWordsFor(refusal.code);
    return std::format("The extension host could not be started: Windows refused {} (error {}{}{}): {}.",
                       row.text,
                       code,
                       words.empty() ? "" : ", ",
                       words,
                       launchErrorText(refusal.error));
}

} // namespace vtextension::wasm::remote::process
