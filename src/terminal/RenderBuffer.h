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

#include <terminal/CellFlags.h>
#include <terminal/Color.h>
#include <terminal/Image.h>
#include <terminal/Grid.h>
#include <terminal/primitives.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

namespace terminal {

struct RenderCell
{
    std::u32string codepoints;
    Coordinate position;
    CellFlags flags;
    RGBColor foregroundColor;
    RGBColor backgroundColor;
    RGBColor decorationColor;
    std::optional<ImageFragment> image;

    bool groupStart = false;
    bool groupEnd = false;
};

struct RenderCursor
{
    Coordinate position;
    CursorShape shape;
    int width;
};

struct RenderBuffer
{
    std::vector<RenderCell> screen{};
    std::optional<RenderCursor> cursor{};
    uint64_t frameID{};

    void clear() { screen.clear(); cursor.reset(); }
};

/// Lock-guarded handle to a read-only RenderBuffer object.
///
/// @see RenderBuffer
struct RenderBufferRef
{
    RenderBuffer const& buffer;
    std::mutex& guard;

    RenderBuffer const& get() const noexcept { return buffer; }

    RenderBufferRef(RenderBuffer const& _buf, std::mutex& _lock):
        buffer{_buf}, guard{_lock}
    {
        guard.lock();
    }

    ~RenderBufferRef()
    {
        guard.unlock();
    }
};

/// Reflects the current state of a RenderDoubleBuffer object.
///
enum class RenderBufferState
{
    WaitingForRefresh,
    RefreshBuffersAndTrySwap,
    TrySwapBuffers
};

constexpr std::string_view to_string(RenderBufferState _state) noexcept
{
    switch (_state)
    {
        case RenderBufferState::WaitingForRefresh: return "WaitingForRefresh";
        case RenderBufferState::RefreshBuffersAndTrySwap: return "RefreshBuffersAndTrySwap";
        case RenderBufferState::TrySwapBuffers: return "TrySwapBuffers";
    }
    return "INVALID";
}

struct RenderDoubleBuffer
{
    std::mutex mutable readerLock;
    std::atomic<size_t> currentBackBufferIndex = 0;
    std::array<RenderBuffer, 2> buffers{};
    std::atomic<RenderBufferState> state = RenderBufferState::WaitingForRefresh;
    std::chrono::steady_clock::time_point lastUpdate{};

    RenderBuffer& backBuffer() noexcept { return buffers[currentBackBufferIndex]; }

    RenderBufferRef frontBuffer() const
    {
        // if (state == RenderBufferState::TrySwapBuffers)
        //     const_cast<RenderDoubleBuffer*>(this)->swapBuffers(lastUpdate);
        RenderBuffer const& frontBuffer = buffers.at((currentBackBufferIndex + 1) % 2);
        return RenderBufferRef(frontBuffer, readerLock);
    }

    void clear()
    {
        backBuffer().clear();
    }

    // Swaps front with back buffer. May only be invoked by the writer thread.
    bool swapBuffers(std::chrono::steady_clock::time_point _now) noexcept;
};

} // end namespace
