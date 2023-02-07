/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <crispy/logstore.h>

#include <fmt/format.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <string_view>

#define BUFFER_OBJECT_INLINE 1

namespace crispy
{

template <typename>
class BufferObject;

template <typename>
class BufferFragment;

template <typename T>
using BufferObjectRelease = std::function<void(BufferObject<T>*)>;

template <typename T>
using BufferObjectPtr = std::shared_ptr<BufferObject<T>>;

auto const inline BufferObjectLog = logstore::Category("BufferObject",
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
template <typename T>
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
#if !defined(BUFFER_OBJECT_INLINE)
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
 * are not gettings its resources deleted but ownership moved
 * back to BufferObjectPool.
 */
template <typename T>
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
template <typename T>
class BufferFragment
{
  public:
    using span_type = gsl::span<T const>;

    BufferFragment(BufferObjectPtr<T> buffer, span_type region) noexcept;

    BufferFragment() noexcept = default;
    BufferFragment(BufferFragment&&) noexcept = default;
    BufferFragment(BufferFragment const&) noexcept = default;
    BufferFragment& operator=(BufferFragment&&) noexcept = default;
    BufferFragment& operator=(BufferFragment const&) noexcept = default;

    void reset() noexcept { _region = {}; }

    void growBy(std::size_t byteCount) noexcept
    {
        _region = span_type(_region.data(), _region.size() + byteCount);
    }

    [[nodiscard]] std::basic_string_view<T> view() const noexcept
    {
        return std::basic_string_view<T>(_region.data(), _region.size());
    }

    [[nodiscard]] span_type span() const noexcept { return _region; }
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
    span_type _region;
};

template <typename T>
BufferFragment(BufferObjectPtr<T>, gsl::span<T const>) -> BufferFragment<T>;

template <typename T>
BufferFragment(BufferObjectPtr<T>, std::basic_string_view<T>) -> BufferFragment<T>;

// {{{ BufferObject implementation
template <typename T>
BufferObject<T>::BufferObject(size_t capacity) noexcept:
#if !defined(BUFFER_OBJECT_INLINE)
    data_ { new T[capacity] },
#endif
    _hotEnd { data() },
    _end { data() + capacity }
{
#if defined(BUFFER_OBJECT_INLINE)
    new (data()) T[capacity];
#endif
    if (BufferObjectLog)
        BufferObjectLog()("Creating BufferObject: {}..{}.", (void*) data(), (void*) end());
}

template <typename T>
BufferObject<T>::~BufferObject()
{
    if (BufferObjectLog)
        BufferObjectLog()("Destroying BufferObject: {}..{}.", (void*) data(), (void*) end());
#if defined(BUFFER_OBJECT_INLINE)
    std::destroy_n(data(), capacity());
#else
    delete[] data_;
#endif
}

template <typename T>
BufferObjectPtr<T> BufferObject<T>::create(size_t capacity, BufferObjectRelease<T> release)
{
#if defined(BUFFER_OBJECT_INLINE)
    auto const totalCapacity = nextPowerOfTwo(static_cast<uint32_t>(sizeof(BufferObject) + capacity));
    auto const nettoCapacity = totalCapacity - sizeof(BufferObject);
    auto ptr = (BufferObject*) malloc(totalCapacity);
    new (ptr) BufferObject(nettoCapacity);
    return BufferObjectPtr<T>(ptr, std::move(release));
#else
    return BufferObjectPtr<T>(new BufferObject<T>(nextPowerOfTwo(capacity)), std::move(release));
#endif
}

template <typename T>
gsl::span<T const> BufferObject<T>::writeAtEnd(gsl::span<T const> data) noexcept
{
    assert(_hotEnd + data.size() <= _end);
    memcpy(_hotEnd, data.data(), data.size());
    return gsl::span<T const> { _hotEnd, data.size() };
}

template <typename T>
void BufferObject<T>::reset() noexcept
{
    _hotEnd = data();
}

template <typename T>
inline T* BufferObject<T>::data() noexcept
{
#if defined(BUFFER_OBJECT_INLINE)
    return (T*) (this + 1);
#else
    return data_;
#endif
}

template <typename T>
inline T const* BufferObject<T>::data() const noexcept
{
#if defined(BUFFER_OBJECT_INLINE)
    return (T*) (this + 1);
#else
    return data_;
#endif
}

template <typename T>
inline gsl::span<T> BufferObject<T>::advance(size_t n) noexcept
{
    assert(_hotEnd + n <= _end);
    auto result = gsl::span<T>(_hotEnd, _hotEnd + n);
    _hotEnd += n;
    return result;
}

template <typename T>
inline void BufferObject<T>::advanceHotEndUntil(T const* ptr) noexcept
{
    assert(_hotEnd <= ptr && ptr <= _end);
    _hotEnd = const_cast<T*>(ptr);
}

template <typename T>
inline void BufferObject<T>::clear() noexcept
{
    _hotEnd = data();
}

template <typename T>
inline BufferFragment<T> BufferObject<T>::ref(std::size_t offset, std::size_t size) noexcept
{
    return BufferFragment<T>(this->shared_from_this(), gsl::span<T const>(this->data() + offset, size));
}
// }}}

// {{{ BufferFragment implementation
template <typename T>
BufferFragment<T>::BufferFragment(BufferObjectPtr<T> buffer, gsl::span<T const> region) noexcept:
    _buffer { std::move(buffer) }, _region { region }
{
    assert(_buffer->begin() <= _region.data() && (_region.data() + _region.size()) <= _buffer->end());
}

template <typename T>
inline std::size_t BufferFragment<T>::startOffset() const noexcept
{
    return static_cast<std::size_t>(std::distance((T const*) _buffer->data(), (T const*) data()));
}

template <typename T>
inline std::size_t BufferFragment<T>::endOffset() const noexcept
{
    return startOffset() + size();
}
// }}}

// {{{ BufferObjectPool implementation
template <typename T>
BufferObjectPool<T>::BufferObjectPool(size_t bufferSize): _bufferSize { bufferSize }
{
    BufferObjectLog()("Creating BufferObject pool with chunk size {}",
                      crispy::humanReadableBytes(bufferSize));
}

template <typename T>
BufferObjectPool<T>::~BufferObjectPool()
{
    _reuseBuffers = false;
}

template <typename T>
size_t BufferObjectPool<T>::unusedBuffers() const noexcept
{
    return _unusedBuffers.size();
}

template <typename T>
void BufferObjectPool<T>::releaseUnusedBuffers()
{
    _reuseBuffers = false;
    _unusedBuffers.clear();
    _reuseBuffers = true;
}

template <typename T>
BufferObjectPtr<T> BufferObjectPool<T>::allocateBufferObject()
{
    if (_unusedBuffers.empty())
        return BufferObject<T>::create(_bufferSize, [this](auto p) { release(p); });

    BufferObjectPtr<T> buffer = std::move(_unusedBuffers.front());
    if (BufferObjectLog)
        BufferObjectLog()("Recycling BufferObject from pool: @{}.", (void*) buffer.get());
    _unusedBuffers.pop_front();
    return buffer;
}

template <typename T>
void BufferObjectPool<T>::release(BufferObject<T>* ptr)
{
    if (_reuseBuffers)
    {
        if (BufferObjectLog)
            BufferObjectLog()("Releasing BufferObject from pool: @{}", (void*) ptr);
        ptr->reset();
        _unusedBuffers.emplace_back(ptr, [this](auto p) { release(p); });
    }
    else
    {
#if defined(BUFFER_OBJECT_INLINE)
        std::destroy_n(ptr, 1);
        free(ptr);
#else
        delete ptr;
#endif
    }
}
// }}}

} // namespace crispy
