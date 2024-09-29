// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.h>

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
class buffer_object;

template <BufferObjectElementType>
class buffer_fragment;

template <BufferObjectElementType T>
using buffer_object_release = std::function<void(buffer_object<T>*)>;

template <BufferObjectElementType T>
using buffer_object_ptr = std::shared_ptr<buffer_object<T>>;

auto const inline bufferObjectLog = logstore::category("BufferObject",
                                                       "Logs buffer object pool activity.",
                                                       logstore::category::state::Disabled,
                                                       logstore::category::visibility::Hidden);

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
class buffer_object: public std::enable_shared_from_this<buffer_object<T>>
{
  public:
    explicit buffer_object(size_t capacity) noexcept;
    ~buffer_object();

    static buffer_object_ptr<T> create(size_t capacity, buffer_object_release<T> release = {});

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

    [[nodiscard]] buffer_fragment<T> ref(std::size_t offset, std::size_t size) noexcept;

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

    friend class buffer_fragment<T>;

    std::mutex _mutex;
};

/**
 * buffer_object_pool manages reusable buffer_object objects.
 *
 * buffer_object objects that are about to be disposed
 * are not gettings its resources deleted but ownership moved
 * back to buffer_object_pool.
 */
template <BufferObjectElementType T>
class buffer_object_pool
{
  public:
    explicit buffer_object_pool(size_t bufferSize = 4096);
    ~buffer_object_pool();

    void releaseUnusedBuffers();
    [[nodiscard]] size_t unusedBuffers() const noexcept;
    [[nodiscard]] buffer_object_ptr<T> allocateBufferObject();

  private:
    void release(buffer_object<T>* ptr);

    bool _reuseBuffers = true;
    size_t _bufferSize;
    std::list<buffer_object_ptr<T>> _unusedBuffers;
};

/**
 * BufferFragment safely holds a reference to a region of buffer_object.
 */
template <BufferObjectElementType T>
class buffer_fragment
{
  public:
    using span_type = gsl::span<T const>;

    buffer_fragment(buffer_object_ptr<T> buffer, span_type region) noexcept;

    buffer_fragment() noexcept = default;
    buffer_fragment(buffer_fragment&&) noexcept = default;
    buffer_fragment(buffer_fragment const&) noexcept = default;
    buffer_fragment& operator=(buffer_fragment&&) noexcept = default;
    buffer_fragment& operator=(buffer_fragment const&) noexcept = default;

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
    [[nodiscard]] buffer_object_ptr<T> const& owner() const noexcept { return _buffer; }

    [[nodiscard]] bool empty() const noexcept { return _region.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return _region.size(); }
    [[nodiscard]] T const* data() const noexcept { return _region.data(); }
    [[nodiscard]] T const& operator[](size_t i) const noexcept { return _region[i]; }

    [[nodiscard]] decltype(auto) begin() noexcept { return _region.begin(); }
    [[nodiscard]] decltype(auto) end() noexcept { return _region.end(); }

    [[nodiscard]] std::size_t startOffset() const noexcept;
    [[nodiscard]] std::size_t endOffset() const noexcept;

  private:
    buffer_object_ptr<T> _buffer;
    span_type _region;
};

template <BufferObjectElementType T>
buffer_fragment(buffer_object_ptr<T>, gsl::span<T const>) -> buffer_fragment<T>;

template <BufferObjectElementType T>
buffer_fragment(buffer_object_ptr<T>, std::basic_string_view<T>) -> buffer_fragment<T>;

// {{{ buffer_object implementation
template <BufferObjectElementType T>
buffer_object<T>::buffer_object(size_t capacity) noexcept:
#if !defined(BUFFER_OBJECT_INLINE)
    data_ { new T[capacity] },
#endif
    _hotEnd { data() },
    _end { data() + capacity }
{
#if defined(BUFFER_OBJECT_INLINE)
    new (data()) T[capacity];
#endif
    if (bufferObjectLog)
        bufferObjectLog()("Creating BufferObject: {}..{}.", (void*) data(), (void*) end());
}

template <BufferObjectElementType T>
buffer_object<T>::~buffer_object()
{
    if (bufferObjectLog)
        bufferObjectLog()("Destroying BufferObject: {}..{}.", (void*) data(), (void*) end());
#if defined(BUFFER_OBJECT_INLINE)
    std::destroy_n(data(), capacity());
#else
    delete[] data_;
#endif
}

template <BufferObjectElementType T>
buffer_object_ptr<T> buffer_object<T>::create(size_t capacity, buffer_object_release<T> release)
{
#if defined(BUFFER_OBJECT_INLINE)
    auto const totalCapacity = nextPowerOfTwo(static_cast<uint32_t>(sizeof(buffer_object) + capacity));
    auto const nettoCapacity = totalCapacity - sizeof(buffer_object);
    auto ptr = (buffer_object*) malloc(totalCapacity);
    new (ptr) buffer_object(nettoCapacity);
    return buffer_object_ptr<T>(ptr, std::move(release));
#else
    return buffer_object_ptr<T>(new buffer_object<T>(nextPowerOfTwo(capacity)), std::move(release));
#endif
}

template <BufferObjectElementType T>
gsl::span<T const> buffer_object<T>::writeAtEnd(gsl::span<T const> data) noexcept
{
    assert(_hotEnd + data.size() <= _end);
    std::memcpy(_hotEnd, data.data(), data.size());
    return gsl::span<T const> { _hotEnd, data.size() };
}

template <BufferObjectElementType T>
void buffer_object<T>::reset() noexcept
{
    _hotEnd = data();
}

template <BufferObjectElementType T>
inline T* buffer_object<T>::data() noexcept
{
#if defined(BUFFER_OBJECT_INLINE)
    return (T*) (this + 1);
#else
    return data_;
#endif
}

template <BufferObjectElementType T>
inline T const* buffer_object<T>::data() const noexcept
{
#if defined(BUFFER_OBJECT_INLINE)
    return (T*) (this + 1);
#else
    return data_;
#endif
}

template <BufferObjectElementType T>
inline gsl::span<T> buffer_object<T>::advance(size_t n) noexcept
{
    assert(_hotEnd + n <= _end);
    auto result = gsl::span<T>(_hotEnd, _hotEnd + n);
    _hotEnd += n;
    return result;
}

template <BufferObjectElementType T>
inline void buffer_object<T>::advanceHotEndUntil(T const* ptr) noexcept
{
    assert(_hotEnd <= ptr && ptr <= _end);
    _hotEnd = const_cast<T*>(ptr);
}

template <BufferObjectElementType T>
inline void buffer_object<T>::clear() noexcept
{
    _hotEnd = data();
}

template <BufferObjectElementType T>
inline buffer_fragment<T> buffer_object<T>::ref(std::size_t offset, std::size_t size) noexcept
{
    return buffer_fragment<T>(this->shared_from_this(), gsl::span<T const>(this->data() + offset, size));
}
// }}}

// {{{ BufferFragment implementation
template <BufferObjectElementType T>
buffer_fragment<T>::buffer_fragment(buffer_object_ptr<T> buffer, gsl::span<T const> region) noexcept:
    _buffer { std::move(buffer) }, _region { region }
{
    assert(_buffer->begin() <= _region.data() && (_region.data() + _region.size()) <= _buffer->end());
}

template <BufferObjectElementType T>
inline std::size_t buffer_fragment<T>::startOffset() const noexcept
{
    return static_cast<std::size_t>(std::distance((T const*) _buffer->data(), (T const*) data()));
}

template <BufferObjectElementType T>
inline std::size_t buffer_fragment<T>::endOffset() const noexcept
{
    return startOffset() + size();
}
// }}}

// {{{ BufferObjectPool implementation
template <BufferObjectElementType T>
buffer_object_pool<T>::buffer_object_pool(size_t bufferSize): _bufferSize { bufferSize }
{
    bufferObjectLog()("Creating BufferObject pool with chunk size {}",
                      crispy::humanReadableBytes(bufferSize));
}

template <BufferObjectElementType T>
buffer_object_pool<T>::~buffer_object_pool()
{
    _reuseBuffers = false;
}

template <BufferObjectElementType T>
size_t buffer_object_pool<T>::unusedBuffers() const noexcept
{
    return _unusedBuffers.size();
}

template <BufferObjectElementType T>
void buffer_object_pool<T>::releaseUnusedBuffers()
{
    _reuseBuffers = false;
    _unusedBuffers.clear();
    _reuseBuffers = true;
}

template <BufferObjectElementType T>
buffer_object_ptr<T> buffer_object_pool<T>::allocateBufferObject()
{
    if (_unusedBuffers.empty())
        return buffer_object<T>::create(_bufferSize, [this](auto p) { release(p); });

    buffer_object_ptr<T> buffer = std::move(_unusedBuffers.front());
    if (bufferObjectLog)
        bufferObjectLog()("Recycling BufferObject from pool: @{}.", (void*) buffer.get());
    _unusedBuffers.pop_front();
    return buffer;
}

template <BufferObjectElementType T>
void buffer_object_pool<T>::release(buffer_object<T>* ptr)
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
