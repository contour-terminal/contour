// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.hpp>

#include <gsl/span>
#include <gsl/span_ext>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>

#define BUFFER_OBJECT_INLINE 1

namespace crispy
{

/// A concept that defines the requirements for a type to be used as a buffer object element type.
/// This concept requires the type to be trivial and standard layout.
///
/// A typical use should be char or char8_t, but can be virtually anything that satisfies the requirements.
template <typename T>
concept BufferObjectElementType = std::is_trivial_v<T> && std::is_standard_layout_v<T>;

template <BufferObjectElementType>
class BufferObject;

template <BufferObjectElementType>
class BufferFragment;

template <BufferObjectElementType T>
using BufferObjectRelease = std::function<void(BufferObject<T>*)>;

template <BufferObjectElementType T>
using BufferObjectPtr = std::shared_ptr<BufferObject<T>>;

auto inline const bufferObjectLog = logstore::Category("BufferObject",
                                                       "Logs buffer object pool activity.",
                                                       logstore::Category::State::Disabled,
                                                       logstore::Category::Visibility::Hidden);

/**
 * BufferObject is the buffer object a Pty's read-call will use to store
 * the read data.
 * This buffer is suitable for efficient reuse.
 *
 * Properties:
 *
 * - Suitable for incrementally filling grid lines sharing the same SGR attributes.
 * - Keeps reference count of how many Line instances are still using this object.
 * - if a call to `Pty.read(BufferObject&)` does not cause any new references
 *   to this buffer for optimized access, then the next call to `Pty.read()`
 *   can start filling at the same offset again.
 *   The offset gets incremented only if new references have been added.
 * - This buffer does not grow or shrink.
 */
template <BufferObjectElementType T>
class BufferObject: public std::enable_shared_from_this<BufferObject<T>>
{
  public:
    explicit BufferObject(size_t capacity) noexcept;
    ~BufferObject();

    static BufferObjectPtr<T> create(size_t capacity, BufferObjectRelease<T> release = {});

    void reset() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return static_cast<std::size_t>(std::distance(data(), end()));
    }

    [[nodiscard]] std::size_t bytesUsed() const noexcept
    {
        return static_cast<std::size_t>(std::distance(data(), hotEnd()));
    }

    [[nodiscard]] std::size_t bytesAvailable() const noexcept
    {
        return static_cast<std::size_t>(std::distance(hotEnd(), end()));
    }

    [[nodiscard]] float loadFactor() const noexcept { return float(bytesUsed()) / float(capacity()); }

    [[nodiscard]] T* data() noexcept;
    [[nodiscard]] T const* data() const noexcept;

    [[nodiscard]] BufferFragment<T> ref(std::size_t offset, std::size_t size) noexcept;

    /// Returns a pointer to the first byte in the internal data storage.
    T* begin() noexcept { return data(); }
    T const* begin() const noexcept { return data(); }

    /// Returns a pointer one byte past the last used byte.
    T* hotEnd() noexcept { return _hotEnd; }
    T const* hotEnd() const noexcept { return _hotEnd; }

    /// Returns a pointer one byte past the underlying storage's last byte.
    T* end() noexcept { return _end; }
    T const* end() const noexcept { return _end; }

    /// Advances the end of the used area by the given amount of bytes.
    gsl::span<T> advance(size_t n) noexcept;

    void advanceHotEndUntil(T const* ptr) noexcept;

    /// Appends the given amount of data to the buffer object
    /// without advancing the end pointer.
    gsl::span<T const> writeAtEnd(gsl::span<T const> data) noexcept;

    void clear() noexcept;

    void lock() { _mutex.lock(); }
    void unlock() { _mutex.unlock(); }

  private:
#ifndef BUFFER_OBJECT_INLINE
    T* data_;
#endif
    T* _hotEnd;
    T* _end;

    friend class BufferFragment<T>;

    std::mutex _mutex;
};

/**
 * BufferObjectPool manages reusable BufferObject objects.
 *
 * BufferObject objects that are about to be disposed
 * are not getting its resources deleted but ownership moved
 * back to BufferObjectPool.
 */
template <BufferObjectElementType T>
class BufferObjectPool
{
  public:
    explicit BufferObjectPool(size_t bufferSize = 4096);
    ~BufferObjectPool();

    void releaseUnusedBuffers();
    [[nodiscard]] size_t unusedBuffers() const noexcept;
    [[nodiscard]] BufferObjectPtr<T> allocateBufferObject();

  private:
    void release(BufferObject<T>* ptr);

    bool _reuseBuffers = true;
    size_t _bufferSize;
    std::list<BufferObjectPtr<T>> _unusedBuffers;
};

/**
 * BufferFragment safely holds a reference to a region of BufferObject.
 */
template <BufferObjectElementType T>
class BufferFragment
{
  public:
    using SpanType = gsl::span<T const>;

    BufferFragment(BufferObjectPtr<T> buffer, SpanType region) noexcept;

    BufferFragment() noexcept = default;
    BufferFragment(BufferFragment&&) noexcept = default;
    BufferFragment(BufferFragment const&) noexcept = default;
    BufferFragment& operator=(BufferFragment&&) noexcept = default;
    BufferFragment& operator=(BufferFragment const&) noexcept = default;

    void reset() noexcept { _region = {}; }

    void growBy(std::size_t byteCount) noexcept
    {
        _region = SpanType(_region.data(), _region.size() + byteCount);
    }

    [[nodiscard]] std::basic_string_view<T> view() const noexcept
    {
        return std::basic_string_view<T>(_region.data(), _region.size());
    }

    [[nodiscard]] SpanType span() const noexcept { return _region; }
    [[nodiscard]] BufferObjectPtr<T> const& owner() const noexcept { return _buffer; }

    [[nodiscard]] bool empty() const noexcept { return _region.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return _region.size(); }
    [[nodiscard]] T const* data() const noexcept { return _region.data(); }
    [[nodiscard]] T const& operator[](size_t i) const noexcept { return _region[i]; }

    [[nodiscard]] decltype(auto) begin() noexcept { return _region.begin(); }
    [[nodiscard]] decltype(auto) end() noexcept { return _region.end(); }

    [[nodiscard]] std::size_t startOffset() const noexcept;
    [[nodiscard]] std::size_t endOffset() const noexcept;

  private:
    BufferObjectPtr<T> _buffer;
    SpanType _region;
};

template <BufferObjectElementType T>
BufferFragment(BufferObjectPtr<T>, gsl::span<T const>) -> BufferFragment<T>;

template <BufferObjectElementType T>
BufferFragment(BufferObjectPtr<T>, std::basic_string_view<T>) -> BufferFragment<T>;

// {{{ BufferObject implementation
template <BufferObjectElementType T>
BufferObject<T>::BufferObject(size_t capacity) noexcept:
#ifndef BUFFER_OBJECT_INLINE
    data_ { new T[capacity] },
#endif
    _hotEnd { data() },
    _end { data() + capacity }
{
#ifdef BUFFER_OBJECT_INLINE
    new (data()) T[capacity];
#endif
    if (bufferObjectLog)
        bufferObjectLog()("Creating BufferObject: {}..{}.", (void*) data(), (void*) end());
}

template <BufferObjectElementType T>
BufferObject<T>::~BufferObject()
{
    if (bufferObjectLog)
        bufferObjectLog()("Destroying BufferObject: {}..{}.", (void*) data(), (void*) end());
#ifdef BUFFER_OBJECT_INLINE
    std::destroy_n(data(), capacity());
#else
    delete[] data_;
#endif
}

template <BufferObjectElementType T>
BufferObjectPtr<T> BufferObject<T>::create(size_t capacity, BufferObjectRelease<T> release)
{
#ifdef BUFFER_OBJECT_INLINE
    auto const totalCapacity = nextPowerOfTwo(static_cast<uint32_t>(sizeof(BufferObject) + capacity));
    auto const nettoCapacity = totalCapacity - sizeof(BufferObject);
    auto ptr = (BufferObject*) malloc(totalCapacity);
    new (ptr) BufferObject(nettoCapacity);
    return BufferObjectPtr<T>(ptr, std::move(release));
#else
    return BufferObjectPtr<T>(new BufferObject<T>(nextPowerOfTwo(capacity)), std::move(release));
#endif
}

template <BufferObjectElementType T>
gsl::span<T const> BufferObject<T>::writeAtEnd(gsl::span<T const> data) noexcept
{
    assert(_hotEnd + data.size() <= _end);
    std::memcpy(_hotEnd, data.data(), data.size());
    return gsl::span<T const> { _hotEnd, data.size() };
}

template <BufferObjectElementType T>
void BufferObject<T>::reset() noexcept
{
    _hotEnd = data();
}

template <BufferObjectElementType T>
inline T* BufferObject<T>::data() noexcept
{
#ifdef BUFFER_OBJECT_INLINE
    return (T*) (this + 1);
#else
    return data_;
#endif
}

template <BufferObjectElementType T>
inline T const* BufferObject<T>::data() const noexcept
{
#ifdef BUFFER_OBJECT_INLINE
    return (T*) (this + 1);
#else
    return data_;
#endif
}

template <BufferObjectElementType T>
inline gsl::span<T> BufferObject<T>::advance(size_t n) noexcept
{
    assert(_hotEnd + n <= _end);
    auto result = gsl::span<T>(_hotEnd, _hotEnd + n);
    _hotEnd += n;
    return result;
}

template <BufferObjectElementType T>
inline void BufferObject<T>::advanceHotEndUntil(T const* ptr) noexcept
{
    assert(_hotEnd <= ptr && ptr <= _end);
    _hotEnd = const_cast<T*>(ptr);
}

template <BufferObjectElementType T>
inline void BufferObject<T>::clear() noexcept
{
    _hotEnd = data();
}

template <BufferObjectElementType T>
inline BufferFragment<T> BufferObject<T>::ref(std::size_t offset, std::size_t size) noexcept
{
    return BufferFragment<T>(this->shared_from_this(), gsl::span<T const>(this->data() + offset, size));
}
// }}}

// {{{ BufferFragment implementation
template <BufferObjectElementType T>
BufferFragment<T>::BufferFragment(BufferObjectPtr<T> buffer, gsl::span<T const> region) noexcept:
    _buffer { std::move(buffer) }, _region { region }
{
    assert(_buffer->begin() <= _region.data() && (_region.data() + _region.size()) <= _buffer->end());
}

template <BufferObjectElementType T>
inline std::size_t BufferFragment<T>::startOffset() const noexcept
{
    return static_cast<std::size_t>(std::distance((T const*) _buffer->data(), (T const*) data()));
}

template <BufferObjectElementType T>
inline std::size_t BufferFragment<T>::endOffset() const noexcept
{
    return startOffset() + size();
}
// }}}

// {{{ BufferObjectPool implementation
template <BufferObjectElementType T>
BufferObjectPool<T>::BufferObjectPool(size_t bufferSize): _bufferSize { bufferSize }
{
    bufferObjectLog()("Creating BufferObject pool with chunk size {}",
                      crispy::humanReadableBytes(bufferSize));
}

template <BufferObjectElementType T>
BufferObjectPool<T>::~BufferObjectPool()
{
    _reuseBuffers = false;
}

template <BufferObjectElementType T>
size_t BufferObjectPool<T>::unusedBuffers() const noexcept
{
    return _unusedBuffers.size();
}

template <BufferObjectElementType T>
void BufferObjectPool<T>::releaseUnusedBuffers()
{
    _reuseBuffers = false;
    _unusedBuffers.clear();
    _reuseBuffers = true;
}

template <BufferObjectElementType T>
BufferObjectPtr<T> BufferObjectPool<T>::allocateBufferObject()
{
    if (_unusedBuffers.empty())
        return BufferObject<T>::create(_bufferSize, [this](auto p) { release(p); });

    BufferObjectPtr<T> buffer = std::move(_unusedBuffers.front());
    if (bufferObjectLog)
        bufferObjectLog()("Recycling BufferObject from pool: @{}.", (void*) buffer.get());
    _unusedBuffers.pop_front();
    return buffer;
}

template <BufferObjectElementType T>
void BufferObjectPool<T>::release(BufferObject<T>* ptr)
{
    if (_reuseBuffers)
    {
        if (bufferObjectLog)
            bufferObjectLog()("Releasing BufferObject from pool: @{}", (void*) ptr);
        ptr->reset();
        _unusedBuffers.emplace_back(ptr, [this](auto p) { release(p); });
    }
    else
    {
#ifdef BUFFER_OBJECT_INLINE
        std::destroy_n(ptr, 1);
        free(ptr);
#else
        delete ptr;
#endif
    }
}
// }}}

} // namespace crispy
