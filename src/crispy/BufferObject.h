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

#include <fmt/format.h>

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

class BufferObject;
class BufferFragment;

using BufferObjectRelease = std::function<void(BufferObject*)>;
using BufferObjectPtr = std::shared_ptr<BufferObject>;

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
class BufferObject: public std::enable_shared_from_this<BufferObject>
{
  public:
    explicit BufferObject(size_t capacity) noexcept;
    ~BufferObject();

    static BufferObjectPtr create(size_t capacity, BufferObjectRelease release = {});

    void reset() noexcept;

    std::size_t capacity() const noexcept { return static_cast<std::size_t>(std::distance(data(), end())); }
    std::size_t bytesUsed() const noexcept
    {
        return static_cast<std::size_t>(std::distance(data(), hotEnd()));
    }
    std::size_t bytesAvailable() const noexcept
    {
        return static_cast<std::size_t>(std::distance(hotEnd(), end()));
    }
    float loadFactor() const noexcept { return float(bytesUsed()) / float(capacity()); }

    char* data() noexcept;
    char const* data() const noexcept;

    BufferFragment ref(std::size_t offset, std::size_t size) noexcept;

    /// Returns a pointer to the first byte in the internal data storage.
    char* begin() noexcept { return data(); }
    char const* begin() const noexcept { return data(); }

    /// Returns a pointer one byte past the last used byte.
    char* hotEnd() noexcept { return hotEnd_; }
    char const* hotEnd() const noexcept { return hotEnd_; }

    /// Returns a pointer one byte past the underlying storage's last byte.
    char* end() noexcept { return end_; }
    char const* end() const noexcept { return end_; }

    /// Advances the end of the used area by the given amount of bytes.
    void advance(size_t n) noexcept;

    void advanceHotEndUntil(char const* ptr) noexcept;

    /// Appends the given amount of data to the buffer object
    /// without advancing the end pointer.
    std::string_view writeAtEnd(std::string_view data) noexcept;

    void clear() noexcept;

    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }

  private:
#if !defined(BUFFER_OBJECT_INLINE)
    char* data_;
#endif
    char* hotEnd_;
    char* end_;

    friend class BufferFragment;

    std::mutex mutex_;
};

/**
 * BufferObjectPool manages reusable BufferObject objects.
 *
 * BufferObject objects that are about to be disposed
 * are not gettings its resources deleted but ownership moved
 * back to BufferObjectPool.
 */
class BufferObjectPool
{
  public:
    explicit BufferObjectPool(size_t bufferSize = 4096);
    ~BufferObjectPool();

    void releaseUnusedBuffers();
    [[nodiscard]] size_t unusedBuffers() const noexcept;
    [[nodiscard]] BufferObjectPtr allocateBufferObject();

  private:
    void release(BufferObject* ptr);

    bool reuseBuffers_ = true;
    size_t bufferSize_;
    std::list<BufferObjectPtr> unusedBuffers_;
};

/**
 * BufferFragment safely holds a reference to a region of BufferObject.
 */
class BufferFragment
{
  public:
    BufferFragment(BufferObjectPtr buffer, std::size_t offset, std::size_t size) noexcept;
    BufferFragment(BufferObjectPtr buffer, std::string_view region) noexcept;

    BufferFragment() noexcept = default;
    BufferFragment(BufferFragment&&) noexcept = default;
    BufferFragment(BufferFragment const&) noexcept = default;
    BufferFragment& operator=(BufferFragment&&) noexcept = default;
    BufferFragment& operator=(BufferFragment const&) noexcept = default;

    void reset() noexcept { region_ = {}; }

    void growBy(std::size_t byteCount) noexcept
    {
        region_ = std::string_view(region_.data(), region_.size() + byteCount);
    }

    [[nodiscard]] std::string_view view() const noexcept { return region_; }
    [[nodiscard]] BufferObjectPtr const& owner() const noexcept { return buffer_; }

    [[nodiscard]] bool empty() const noexcept { return region_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return region_.size(); }
    [[nodiscard]] char const* data() const noexcept { return region_.data(); }
    [[nodiscard]] char operator[](size_t i) const noexcept { return region_[i]; }

    [[nodiscard]] decltype(auto) begin() noexcept { return region_.begin(); }
    [[nodiscard]] decltype(auto) end() noexcept { return region_.end(); }

    [[nodiscard]] std::size_t startOffset() const noexcept;
    [[nodiscard]] std::size_t endOffset() const noexcept;

  private:
    BufferObjectPtr buffer_;
    std::string_view region_;
};

// {{{ BufferObject inlines
inline char* BufferObject::data() noexcept
{
#if defined(BUFFER_OBJECT_INLINE)
    return (char*) (this + 1);
#else
    return data_;
#endif
}

inline char const* BufferObject::data() const noexcept
{
#if defined(BUFFER_OBJECT_INLINE)
    return (char*) (this + 1);
#else
    return data_;
#endif
}

inline void BufferObject::advance(size_t n) noexcept
{
    assert(hotEnd_ + n <= end_);
    hotEnd_ += n;
}

inline void BufferObject::advanceHotEndUntil(char const* ptr) noexcept
{
    assert(hotEnd_ <= ptr && ptr <= end_);
    hotEnd_ = const_cast<char*>(ptr);
}

inline void BufferObject::clear() noexcept
{
    hotEnd_ = data();
}

inline BufferFragment BufferObject::ref(std::size_t offset, std::size_t size) noexcept
{
    return BufferFragment(shared_from_this(), offset, size);
}
// }}}

// {{{ BufferFragment inlines
inline std::size_t BufferFragment::startOffset() const noexcept
{
    return static_cast<std::size_t>(std::distance((char*) buffer_->data(), (char*) data()));
}

inline std::size_t BufferFragment::endOffset() const noexcept
{
    return startOffset() + size();
}
// }}}

} // namespace crispy
