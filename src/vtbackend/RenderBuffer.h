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

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/Grid.h>
#include <vtbackend/Image.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/RenderTarget.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

namespace terminal
{

struct render_attributes
{
    rgb_color foregroundColor {};
    rgb_color backgroundColor {};
    rgb_color decorationColor {};
    cell_flags flags {};
};

/**
 * Renderable representation of a grid cell with color-altering pre-applied and
 * additional information for cell ranges that can be text-shaped together.
 */
struct render_cell
{
    std::u32string codepoints;
    std::shared_ptr<image_fragment> image;
    cell_location position;
    render_attributes attributes;
    uint8_t width = 1;

    bool groupStart = false;
    bool groupEnd = false;
};

/**
 * Renderable representation of a grid line with monochrome SGR styling.
 */
struct render_line
{
    std::string_view text;
    line_offset lineOffset;
    ColumnCount usedColumns;
    ColumnCount displayWidth;
    render_attributes textAttributes;
    render_attributes fillAttributes;
};

struct render_cursor
{
    cell_location position;
    cursor_shape shape;
    int width = 1;
};

struct render_buffer
{
    std::vector<render_cell> cells {};
    std::vector<render_line> lines {};
    std::optional<render_cursor> cursor {};
    uint64_t frameID {};

    void clear()
    {
        cells.clear();
        lines.clear();
        cursor.reset();
    }
};

/// Lock-guarded handle to a read-only RenderBuffer object.
///
/// @see RenderBuffer
struct render_buffer_ref
{
    render_buffer const& buffer;
    std::mutex& guard;

    [[nodiscard]] render_buffer const& get() const noexcept { return buffer; }

    render_buffer_ref(render_buffer const& buf, std::mutex& lock): buffer { buf }, guard { lock }
    {
        guard.lock();
    }

    ~render_buffer_ref() { guard.unlock(); }
};

/// Reflects the current state of a RenderDoubleBuffer object.
///
enum class render_buffer_state
{
    WaitingForRefresh,
    RefreshBuffersAndTrySwap,
    TrySwapBuffers
};

constexpr std::string_view to_string(render_buffer_state state) noexcept
{
    switch (state)
    {
        case render_buffer_state::WaitingForRefresh: return "WaitingForRefresh";
        case render_buffer_state::RefreshBuffersAndTrySwap: return "RefreshBuffersAndTrySwap";
        case render_buffer_state::TrySwapBuffers: return "TrySwapBuffers";
    }
    return "INVALID";
}

struct render_double_buffer
{
    std::mutex mutable readerLock;
    std::atomic<size_t> currentBackBufferIndex = 0;
    std::array<render_buffer, 2> buffers {};
    std::atomic<render_buffer_state> state = render_buffer_state::WaitingForRefresh;
    std::chrono::steady_clock::time_point lastUpdate {};

    render_buffer& backBuffer() noexcept { return buffers[currentBackBufferIndex]; }

    render_buffer_ref frontBuffer() const
    {
        // if (state == RenderBufferState::TrySwapBuffers)
        //     const_cast<RenderDoubleBuffer*>(this)->swapBuffers(lastUpdate);
        render_buffer const& frontBuffer = buffers.at((currentBackBufferIndex + 1) % 2);
        return render_buffer_ref(frontBuffer, readerLock);
    }

    void clear() { backBuffer().clear(); }

    // Swaps front with back buffer. May only be invoked by the writer thread.
    bool swapBuffers(std::chrono::steady_clock::time_point now) noexcept;
};

} // namespace terminal
