// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `UniqueCoroHandle` — a move-only RAII owner of a coroutine handle.
///
/// `Task`, `whenAll`'s child runner, and `whenAny`'s child runner all own a
/// `std::coroutine_handle` with identical discipline: destroy the frame on
/// destruction or overwrite, transfer ownership on move, forbid copying. This
/// single-sources that ownership so the rule lives in one place rather than
/// drifting between the two combinators and the core task type.

#include <coroutine>
#include <utility>

namespace coro::detail
{

/// Move-only owner of a `std::coroutine_handle<Promise>`: destroys the owned
/// frame on destruction or move-assignment, transfers ownership on move, and
/// forbids copying.
/// @tparam Promise The coroutine's promise type.
template <typename Promise>
class UniqueCoroHandle
{
  public:
    using handle_type = std::coroutine_handle<Promise>;

    UniqueCoroHandle() noexcept = default;

    /// @param handle The coroutine frame to own (empty by default).
    explicit UniqueCoroHandle(handle_type handle) noexcept: _handle(handle) {}

    UniqueCoroHandle(UniqueCoroHandle&& other) noexcept: _handle(std::exchange(other._handle, {})) {}

    UniqueCoroHandle& operator=(UniqueCoroHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle)
                _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    UniqueCoroHandle(UniqueCoroHandle const&) = delete;
    UniqueCoroHandle& operator=(UniqueCoroHandle const&) = delete;

    ~UniqueCoroHandle()
    {
        if (_handle)
            _handle.destroy();
    }

    /// @return The owned handle (empty when none is owned).
    [[nodiscard]] handle_type get() const noexcept { return _handle; }

    /// @return True when a frame is owned.
    explicit operator bool() const noexcept { return static_cast<bool>(_handle); }

  private:
    handle_type _handle {};
};

} // namespace coro::detail
