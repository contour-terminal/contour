// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <deque>
    #include <format>
    #include <memory>
    #include <mutex>
    #include <utility>

    #include <unistd.h>

    // glibc's <unistd.h> declares `environ` (C++ builds define _GNU_SOURCE); macOS reaches it
    // through _NSGetEnviron(), and the other systems want it declared here.
    #ifdef __APPLE__
        #include <crt_externs.h>
    #elifndef __GLIBC__
extern "C" char** environ;
    #endif
#endif

namespace core
{

namespace
{
#ifndef _WIN32
    /// Serializes this translation unit's reads of the environment block against one another and
    /// against its writer, setProcessEnvironmentVariable().
    ///
    /// Process-wide rather than a member, because what it guards is process-wide: two
    /// LiveEnvironment instances read the same block, so a per-instance lock would serialize
    /// nothing.
    [[nodiscard]] std::mutex& environmentMutex() noexcept
    {
        static std::mutex instance;
        return instance;
    }

    /// @return The process's environment block, as `getenv()` and `execvp()` see it.
    [[nodiscard]] char**& processEnviron() noexcept
    {
    #ifdef __APPLE__
        return *_NSGetEnviron();
    #else
        return environ;
    #endif
    }

    /// Looks a name up in the environment block as it stands right now.
    ///
    /// A scan of the block rather than a getenv() call, which does exactly this scan and no better:
    /// glibc's getenv() walks the same array, so a hand-rolled walk is neither slower nor less safe.
    /// It also avoids the thread-unsafe getenv() that this project's clang-tidy configuration
    /// rejects outright.
    ///
    /// @param name Name of the variable to look up.
    /// @return Its value, or std::nullopt if it is not set.
    [[nodiscard]] std::optional<std::string> lookupInEnviron(std::string_view name)
    {
        auto* const* entry = processEnviron();
        while (entry != nullptr && *entry != nullptr)
        {
            auto const line = std::string_view { *entry };
            if (auto const separator = line.find('=');
                separator != std::string_view::npos && line.substr(0, separator) == name)
                return std::string { line.substr(separator + 1) };
            ++entry;
        }
        return std::nullopt;
    }

    /// @return Whether @p entry, a "name=value" line of an environment block, is the one for @p name.
    [[nodiscard]] bool isEntryFor(std::string_view entry, std::string_view name) noexcept
    {
        return entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=';
    }

    /// What setProcessEnvironmentVariable() has published: every block, and every entry it created.
    ///
    /// A deque of each, because a deque never moves what it holds, so a pointer into one stays
    /// valid for as long as the deque lives -- which is the rest of the process; see
    /// publishedEnvironment().
    struct PublishedEnvironment
    {
        std::deque<std::string> entries;       ///< "name=value" lines the writer created.
        std::deque<std::vector<char*>> blocks; ///< NUL-terminated blocks, each published once.
    };

    /// @return The process's one @c PublishedEnvironment, which is never destroyed.
    ///
    /// Never destroyed on purpose: `environ` points into it until the process ends, and a static
    /// destructor or an `atexit()` handler that runs after it would have been destroyed may still
    /// read the environment -- through `getenv()`, or through a block libc's own `setenv()` copied
    /// from one of these, whose entries still point into it.
    [[nodiscard]] PublishedEnvironment& publishedEnvironment()
    {
        static auto& instance = *std::make_unique<PublishedEnvironment>().release();
        return instance;
    }

    /// Publishes a copy of the current environment block in which @p name is removed and, when
    /// @p value is given, set to it.
    /// @param name Name of the variable to change, valid by isValidName().
    /// @param value The value to set, or std::nullopt to remove the variable.
    void publishEnvironment(std::string_view name, std::optional<std::string_view> value)
    {
        auto const lock = std::scoped_lock { environmentMutex() };
        auto& published = publishedEnvironment();

        auto block = std::vector<char*> {};
        auto removed = false;
        auto unchanged = false;
        auto* const* entry = processEnviron();
        while (entry != nullptr && *entry != nullptr)
        {
            if (isEntryFor(*entry, name))
            {
                // Only a first and single entry that already reads the value leaves nothing to do.
                // Readers take the first entry, and a second one (a block inherited through
                // execve() can name a variable twice) is dropped by publishing.
                unchanged =
                    !removed && value && std::string_view { *entry }.substr(name.size() + 1) == *value;
                removed = true;
            }
            else
                block.push_back(*entry);
            ++entry;
        }
        // Nothing to remove, or one entry that already reads so: the block as it stands is the
        // answer, and publishing a copy would cost a block that is never freed.
        if (value ? unchanged : !removed)
            return;

        if (value)
            block.push_back(published.entries.emplace_back(std::format("{}={}", name, *value)).data());
        block.push_back(nullptr);
        processEnviron() = published.blocks.emplace_back(std::move(block)).data();
    }
#else
    /// @return @p text in UTF-16, or std::nullopt where it is not UTF-8 -- which the wide API has
    ///         no spelling for, where the code-page one took each byte as whatever character the
    ///         machine's code page gives it.
    [[nodiscard]] std::optional<std::wstring> toWide(std::string const& text)
    {
        if (text.empty())
            return std::wstring {};
        auto const size = static_cast<int>(text.size());
        auto const length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
        if (length <= 0)
            return std::nullopt;
        auto wide = std::wstring(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, wide.data(), length);
        return wide;
    }

    /// @return @p text in UTF-8. A lone surrogate, which UTF-8 cannot spell, reads as U+FFFD.
    [[nodiscard]] std::string toUtf8(std::wstring const& text)
    {
        if (text.empty())
            return {};
        auto const size = static_cast<int>(text.size());
        auto const length = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
        auto utf8 = std::string(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), size, utf8.data(), length, nullptr, nullptr);
        return utf8;
    }
#endif

    /// @return Whether @p name can name a variable of an environment block: it is not empty, and
    ///         has neither the '=' that ends a name nor the NUL that ends an entry.
    [[nodiscard]] bool isValidName(std::string_view name) noexcept
    {
        return !name.empty() && !name.contains('=') && !name.contains('\0');
    }
} // namespace

std::optional<std::string> LiveEnvironment::get(std::string_view name) const
{
#ifdef _WIN32
    // The wide API, converting to and from UTF-8: the block is UTF-16, and the code-page API
    // mangles whatever the machine's code page cannot spell -- a user profile path with an umlaut
    // on a machine whose code page has none, anything outside Latin on most (core-cpp#7). The
    // conversion also gives the name the terminating NUL a string_view does not promise.
    auto const wideName = toWide(std::string { name });
    if (!wideName || wideName->empty())
        return std::nullopt;

    // The Win32 block rather than the CRT's copy of it: SetEnvironmentVariable() writes the former
    // and the operating system synchronizes reads of it, whereas the CRT copy is only refreshed by
    // the CRT's own setters.
    auto const required = GetEnvironmentVariableW(wideName->c_str(), nullptr, 0);
    if (required == 0)
        return std::nullopt;

    // `required` counts the terminating NUL; the second call's result does not. A writer racing
    // between the two calls can shrink the value, so the second length is the one to trust. A
    // result of 0 is an empty value unless the call says the variable is gone: an empty value is a
    // variable that is set, as it is on POSIX.
    auto buffer = std::vector<wchar_t>(required);
    SetLastError(ERROR_SUCCESS);
    auto const written = GetEnvironmentVariableW(wideName->c_str(), buffer.data(), required);
    if (written >= required || (written == 0 && GetLastError() != ERROR_SUCCESS))
        return std::nullopt;
    return toUtf8(std::wstring { buffer.data(), written });
#else
    // The copy has to happen under the lock, not after it: the block holds pointers that a
    // concurrent setenv() may reallocate out from under a reader.
    auto const lock = std::scoped_lock { environmentMutex() };
    return lookupInEnviron(name);
#endif
}

CachingEnvironment::CachingEnvironment(Environment const& source) noexcept: _source { source }
{
}

std::optional<std::string> CachingEnvironment::get(std::string_view name) const
{
    // The source is consulted under this lock as well, so two threads racing on the same unseen
    // name read it once rather than twice. The two mutexes are only ever taken in this order.
    auto const lock = std::scoped_lock { _mutex };

    if (auto const i = _cache.find(name); i != _cache.end())
        return i->second;

    return _cache.emplace(name, _source.get(name)).first->second;
}

Environment& defaultEnvironment()
{
    static LiveEnvironment const source;
    static CachingEnvironment instance { source };
    return instance;
}

std::expected<void, std::error_code> setProcessEnvironmentVariable(std::string_view name,
                                                                   std::string_view value)
{
    if (!isValidName(name) || value.contains('\0'))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

#ifdef _WIN32
    // In UTF-16, as `LiveEnvironment::get` reads it; text that is not UTF-8 has no spelling there.
    auto const wideName = toWide(std::string { name });
    auto const wideValue = toWide(std::string { value });
    if (!wideName || !wideValue)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (SetEnvironmentVariableW(wideName->c_str(), wideValue->c_str()) == 0)
        return std::unexpected(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
#else
    publishEnvironment(name, value);
#endif
    return {};
}

std::expected<void, std::error_code> unsetProcessEnvironmentVariable(std::string_view name)
{
    if (!isValidName(name))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

#ifdef _WIN32
    auto const wideName = toWide(std::string { name });
    if (!wideName)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (SetEnvironmentVariableW(wideName->c_str(), nullptr) == 0)
    {
        // Removing a variable that is not set leaves it unset, which is what was asked for.
        if (auto const error = GetLastError(); error != ERROR_ENVVAR_NOT_FOUND)
            return std::unexpected(std::error_code(static_cast<int>(error), std::system_category()));
    }
#else
    publishEnvironment(name, std::nullopt);
#endif
    return {};
}

} // namespace core
