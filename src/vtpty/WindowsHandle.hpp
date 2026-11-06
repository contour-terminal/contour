// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Windows handle plumbing shared by vtpty and the modules above it: owning a handle, the last error, a
/// process attribute list, and ntdll's functions. A handle crosses to another process as a CrossingHandle.

#ifdef _WIN32

    #include <cstddef>
    #include <cstdint>
    #include <expected>
    #include <memory>
    #include <span>
    #include <system_error>
    #include <utility>

    #include <Windows.h>

namespace vtpty
{

/// Owns a Win32 HANDLE and closes it on destruction.
class UniqueHandle
{
  public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept: _handle { handle } {}
    UniqueHandle(UniqueHandle const&) = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept: _handle { std::exchange(other._handle, nullptr) } {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _handle = std::exchange(other._handle, nullptr);
        }
        return *this;
    }
    ~UniqueHandle() { reset(); }

    /// @return The handle, still owned.
    [[nodiscard]] HANDLE get() const noexcept { return _handle; }

    /// @return Whether it owns a handle.
    [[nodiscard]] bool valid() const noexcept
    {
        return _handle != nullptr && _handle != INVALID_HANDLE_VALUE;
    }

    /// Closes the handle, if it owns one.
    void reset() noexcept
    {
        if (valid())
            CloseHandle(_handle);
        _handle = nullptr;
    }

  private:
    HANDLE _handle = nullptr;
};

/// @return @p code as the system error it is.
[[nodiscard]] inline std::error_code errorFrom(DWORD code) noexcept
{
    return std::error_code { static_cast<int>(code), std::system_category() };
}

/// @return Why the last call of this thread failed.
[[nodiscard]] inline std::error_code lastError() noexcept
{
    return errorFrom(GetLastError());
}

/// Whether a process this one starts with a handle in its handle list inherits it.
enum class HandleInheritance : uint8_t
{
    NotInherited =
        0,     ///< It does not: what every handle this process makes is, and what an adopted one becomes.
    Inherited, ///< It does.
};

/// Sets whether a process this one starts may inherit @p handle.
/// @return Nothing; or why it could not be set.
[[nodiscard]] inline std::expected<void, std::error_code> setInheritance(
    HANDLE handle, HandleInheritance inheritance) noexcept
{
    if (SetHandleInformation(handle,
                             HANDLE_FLAG_INHERIT,
                             inheritance == HandleInheritance::Inherited ? HANDLE_FLAG_INHERIT : 0)
        == FALSE)
        return std::unexpected(lastError());
    return {};
}

/// @return The ntdll function @p name, of type @p Function; null where this Windows has none.
template <typename Function>
[[nodiscard]] Function ntdllFunction(char const* name) noexcept
{
    return reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
}

/// Owns an initialized PROC_THREAD_ATTRIBUTE_LIST.
class AttributeList
{
  public:
    /// @param attributeCount Number of attributes the list will hold.
    /// @return The initialized list, or why it could not be created.
    [[nodiscard]] static std::expected<AttributeList, std::error_code> create(DWORD attributeCount)
    {
        auto size = SIZE_T { 0 };
        InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &size); // fails, yields size
        auto storage = std::make_unique<std::byte[]>(size);
        auto* const list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.get());
        if (!InitializeProcThreadAttributeList(list, attributeCount, 0, &size))
            return std::unexpected(lastError());
        return AttributeList { std::move(storage), list };
    }

    AttributeList(AttributeList const&) = delete;
    AttributeList& operator=(AttributeList const&) = delete;
    AttributeList(AttributeList&& other) noexcept:
        _storage { std::move(other._storage) }, _list { std::exchange(other._list, nullptr) }
    {
    }
    AttributeList& operator=(AttributeList&&) = delete;
    ~AttributeList()
    {
        if (_list != nullptr)
            DeleteProcThreadAttributeList(_list);
    }

    /// @return The list.
    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return _list; }

    /// Has a process started with this list inherit exactly @p handles, and nothing else.
    /// @param handles The handles; each must be inheritable when the process is started.
    /// @return Nothing, or why the list refused them.
    [[nodiscard]] std::expected<void, std::error_code> inheritOnly(std::span<HANDLE> handles)
    {
        if (UpdateProcThreadAttribute(_list,
                                      0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      handles.data(),
                                      handles.size() * sizeof(HANDLE),
                                      nullptr,
                                      nullptr)
            == FALSE)
            return std::unexpected(lastError());
        return {};
    }

  private:
    AttributeList(std::unique_ptr<std::byte[]> storage, LPPROC_THREAD_ATTRIBUTE_LIST list) noexcept:
        _storage { std::move(storage) }, _list { list }
    {
    }

    std::unique_ptr<std::byte[]> _storage;
    LPPROC_THREAD_ATTRIBUTE_LIST _list = nullptr;
};

} // namespace vtpty

#endif
