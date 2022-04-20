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
#include <crispy/BufferObject.h>
#include <crispy/logstore.h>

#include <fmt/format.h>

#include <cassert>

using std::destroy_n;
using std::move;
using std::size_t;
using std::string_view;

namespace crispy
{

auto const inline BufferObjectLog = logstore::Category("BufferObject",
                                                       "Logs buffer object pool activity.",
                                                       logstore::Category::State::Disabled,
                                                       logstore::Category::Visibility::Hidden);

BufferFragment::BufferFragment(BufferObjectPtr buffer, size_t offset, size_t size) noexcept:
    buffer_ { move(buffer) }, region_ { buffer_->data() + offset, size }
{
    assert(buffer_->begin() <= &*region_.begin() && &*region_.end() <= buffer_->end());
}

BufferFragment::BufferFragment(BufferObjectPtr buffer, string_view region) noexcept:
    buffer_ { move(buffer) }, region_ { region }
{
    assert(buffer_->begin() <= &*region_.begin() && &*region_.end() <= buffer_->end());
}

BufferObject::BufferObject(size_t capacity) noexcept:
#if !defined(BUFFER_OBJECT_INLINE)
    data_ { new char[capacity] },
#endif
    hotEnd_ { data() },
    end_ { data() + capacity }
{
#if defined(BUFFER_OBJECT_INLINE)
    new (data()) char[capacity];
#endif
    if (BufferObjectLog)
        BufferObjectLog()("Creating BufferObject: {}..{}.", (void*) data(), (void*) end());
}

BufferObject::~BufferObject()
{
    if (BufferObjectLog)
        BufferObjectLog()("Destroying BufferObject: {}..{}.", (void*) data(), (void*) end());
#if !defined(BUFFER_OBJECT_INLINE)
    delete[] data_;
#endif
}

BufferObjectPtr BufferObject::create(size_t capacity, BufferObjectRelease release)
{
#if defined(BUFFER_OBJECT_INLINE)
    auto const totalCapacity = nextPowerOfTwo(static_cast<uint32_t>(sizeof(BufferObject) + capacity));
    auto const nettoCapacity = totalCapacity - sizeof(BufferObject);
    auto ptr = (BufferObject*) malloc(totalCapacity);
    new (ptr) BufferObject(nettoCapacity);
    return BufferObjectPtr(ptr, move(release));
#else
    return BufferObjectPtr(new BufferObject(nextPowerOfTwo(capacity)), move(release));
#endif
}

string_view BufferObject::writeAtEnd(string_view data) noexcept
{
    assert(hotEnd_ + data.size() <= end_);
    memcpy(hotEnd_, data.data(), data.size());
    return string_view { hotEnd_, data.size() };
}

BufferObjectPool::BufferObjectPool(size_t bufferSize): bufferSize_ { bufferSize }
{
    BufferObjectLog()("Creating BufferObject pool with chunk size {}",
                      crispy::humanReadableBytes(bufferSize));
}

BufferObjectPool::~BufferObjectPool()
{
    reuseBuffers_ = false;
}

size_t BufferObjectPool::unusedBuffers() const noexcept
{
    return unusedBuffers_.size();
}

void BufferObjectPool::releaseUnusedBuffers()
{
    reuseBuffers_ = false;
    unusedBuffers_.clear();
    reuseBuffers_ = true;
}

BufferObjectPtr BufferObjectPool::allocateBufferObject()
{
    if (unusedBuffers_.empty())
        return BufferObject::create(bufferSize_, [this](auto p) { release(p); });

    BufferObjectPtr buffer = move(unusedBuffers_.front());
    if (BufferObjectLog)
        BufferObjectLog()("Recycling BufferObject from pool: @{}.", (void*) buffer.get());
    unusedBuffers_.pop_front();
    return buffer;
}

void BufferObject::reset() noexcept
{
    hotEnd_ = data();
}

void BufferObjectPool::release(BufferObject* ptr)
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
        destroy_n(ptr, 1);
        free(ptr);
#else
        delete ptr;
#endif
    }
}

} // namespace crispy
