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
    T* hotEnd() noexcept { return hotEnd_; }
    T const* hotEnd() const noexcept { return hotEnd_; }

    /// Returns a pointer one byte past the underlying storage's last byte.
    T* end() noexcept { return end_; }
    T const* end() const noexcept { return end_; }

    /// Advances the end of the used area by the given amount of bytes.
    void advance(size_t n) noexcept;

    void advanceHotEndUntil(T const* ptr) noexcept;

    /// Appends the given amount of data to the buffer object
    /// without advancing the end pointer.
    gsl::span<T const> writeAtEnd(gsl::span<T const> data) noexcept;

    void clear() noexcept;

    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }

  private:
#if !defined(BUFFER_OBJECT_INLINE)
    T* data_;
#endif
    T* hotEnd_;
    T* end_;

    friend class BufferFragment<T>;

    std::mutex mutex_;
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

    bool reuseBuffers_ = true;
    size_t bufferSize_;
    std::list<BufferObjectPtr<T>> unusedBuffers_;
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

    void reset() noexcept { region_ = {}; }

    void growBy(std::size_t byteCount) noexcept
    {
        region_ = span_type(region_.data(), region_.size() + byteCount);
    }

    [[nodiscard]] std::basic_string_view<T> view() const noexcept
    {
        return std::basic_string_view<T>(region_.data(), region_.size());
    }

    [[nodiscard]] span_type span() const noexcept { return region_; }
    [[nodiscard]] BufferObjectPtr<T> const& owner() const noexcept { return buffer_; }

    [[nodiscard]] bool empty() const noexcept { return region_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return region_.size(); }
    [[nodiscard]] T const* data() const noexcept { return region_.data(); }
    [[nodiscard]] T const& operator[](size_t i) const noexcept { return region_[i]; }

    [[nodiscard]] decltype(auto) begin() noexcept { return region_.begin(); }
    [[nodiscard]] decltype(auto) end() noexcept { return region_.end(); }

    [[nodiscard]] std::size_t startOffset() const noexcept;
    [[nodiscard]] std::size_t endOffset() const noexcept;

  private:
    BufferObjectPtr<T> buffer_;
    span_type region_;
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
    hotEnd_ { data() },
    end_ { data() + capacity }
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
    assert(hotEnd_ + data.size() <= end_);
    memcpy(hotEnd_, data.data(), data.size());
    return gsl::span<T const> { hotEnd_, data.size() };
}

template <typename T>
void BufferObject<T>::reset() noexcept
{
    hotEnd_ = data();
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
inline void BufferObject<T>::advance(size_t n) noexcept
{
    assert(hotEnd_ + n <= end_);
    hotEnd_ += n;
}

template <typename T>
inline void BufferObject<T>::advanceHotEndUntil(T const* ptr) noexcept
{
    assert(hotEnd_ <= ptr && ptr <= end_);
    hotEnd_ = const_cast<T*>(ptr);
}

template <typename T>
inline void BufferObject<T>::clear() noexcept
{
    hotEnd_ = data();
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
    buffer_ { std::move(buffer) }, region_ { region }
{
    assert(buffer_->begin() <= region_.data() && (region_.data() + region_.size()) <= buffer_->end());
}

template <typename T>
inline std::size_t BufferFragment<T>::startOffset() const noexcept
{
    return static_cast<std::size_t>(std::distance((T const*) buffer_->data(), (T const*) data()));
}

template <typename T>
inline std::size_t BufferFragment<T>::endOffset() const noexcept
{
    return startOffset() + size();
}
// }}}

// {{{ BufferObjectPool implementation
template <typename T>
BufferObjectPool<T>::BufferObjectPool(size_t bufferSize): bufferSize_ { bufferSize }
{
    BufferObjectLog()("Creating BufferObject pool with chunk size {}",
                      crispy::humanReadableBytes(bufferSize));
}

template <typename T>
BufferObjectPool<T>::~BufferObjectPool()
{
    reuseBuffers_ = false;
}

template <typename T>
size_t BufferObjectPool<T>::unusedBuffers() const noexcept
{
    return unusedBuffers_.size();
}

template <typename T>
void BufferObjectPool<T>::releaseUnusedBuffers()
{
    reuseBuffers_ = false;
    unusedBuffers_.clear();
    reuseBuffers_ = true;
}

template <typename T>
BufferObjectPtr<T> BufferObjectPool<T>::allocateBufferObject()
{
    if (unusedBuffers_.empty())
        return BufferObject<T>::create(bufferSize_, [this](auto p) { release(p); });

    BufferObjectPtr<T> buffer = std::move(unusedBuffers_.front());
    if (BufferObjectLog)
        BufferObjectLog()("Recycling BufferObject from pool: @{}.", (void*) buffer.get());
    unusedBuffers_.pop_front();
    return buffer;
}

template <typename T>
void BufferObjectPool<T>::release(BufferObject<T>* ptr)
{
    if (reuseBuffers_)
    {
        if (BufferObjectLog)
            BufferObjectLog()("Releasing BufferObject from pool: @{}", (void*) ptr);
        ptr->reset();
        unusedBuffers_.emplace_back(ptr, [this](auto p) { release(p); });
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
