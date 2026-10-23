// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <version>

namespace vtextension
{

/// A shared pointer that one thread publishes and others read, without either waiting on a lock the
/// other may hold for long: `std::atomic<std::shared_ptr<T>>` where the standard library has it.
///
/// libc++ does not implement `std::atomic<std::shared_ptr>` yet; there a mutex guards the pointer,
/// held only for the copy of one shared pointer.
template <typename T>
class AtomicSharedPtr
{
  public:
    AtomicSharedPtr() = default;
    AtomicSharedPtr(AtomicSharedPtr const&) = delete;
    AtomicSharedPtr(AtomicSharedPtr&&) = delete;
    AtomicSharedPtr& operator=(AtomicSharedPtr const&) = delete;
    AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;
    ~AtomicSharedPtr() = default;

    /// @return The pointer last stored; null before the first store.
    [[nodiscard]] std::shared_ptr<T> load() const noexcept
    {
#ifdef __cpp_lib_atomic_shared_ptr
        return _pointer.load(std::memory_order_acquire);
#else
        auto const lock = std::scoped_lock { _mutex };
        return _pointer;
#endif
    }

    /// Publishes @p pointer.
    /// @param pointer What later loads return.
    void store(std::shared_ptr<T> pointer) noexcept
    {
#ifdef __cpp_lib_atomic_shared_ptr
        _pointer.store(std::move(pointer), std::memory_order_release);
#else
        auto const lock = std::scoped_lock { _mutex };
        std::swap(_pointer, pointer);
        // The previous pointer is released after the lock, by `pointer`'s destructor.
#endif
    }

  private:
#ifdef __cpp_lib_atomic_shared_ptr
    std::atomic<std::shared_ptr<T>> _pointer;
#else
    mutable std::mutex _mutex;
    std::shared_ptr<T> _pointer;
#endif
};

} // namespace vtextension
