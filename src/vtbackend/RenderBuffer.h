// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/Grid.h>
#include <vtbackend/Image.h>
#include <vtbackend/Line.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/RenderTarget.h>

#include <gsl/pointers>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

namespace vtbackend
{

struct RenderAttributes
{
    RGBColor foregroundColor {};
    RGBColor backgroundColor {};
    RGBColor decorationColor {};
    CellFlags flags {};
    LineFlags lineFlags = LineFlag::None;
};

/**
 * Renderable representation of a grid cell with color-altering pre-applied and
 * additional information for cell ranges that can be text-shaped together.
 */
struct RenderCell
{
    std::u32string codepoints;
    std::shared_ptr<ImageFragment> image;
    CellLocation position;
    RenderAttributes attributes;
    uint8_t width = 1;

    bool groupStart = false;
    bool groupEnd = false;
};

/**
 * Renderable representation of a grid line with monochrome SGR styling.
 */
struct RenderLine
{
    std::string_view text;
    LineOffset lineOffset;
    ColumnCount usedColumns;
    ColumnCount displayWidth;
    RenderAttributes textAttributes;
    RenderAttributes fillAttributes;
    LineFlags flags = LineFlag::None;
};

struct RenderCursor
{
    CellLocation position;
    CursorShape shape;
    int width = 1;
    std::optional<CellLocation> animateFrom {};  ///< Grid position cursor is animating from.
    float animationProgress = 1.0f;              ///< 0.0 = at animateFrom, 1.0 = at position.
    RGBColor cursorColor {};                     ///< Resolved cursor color at target position.
    std::optional<RGBColor> animateFromColor {}; ///< Cursor color at animation source position.
};

struct RenderBuffer
{
    std::vector<RenderCell> cells {};
    std::vector<RenderLine> lines {};
    std::optional<RenderCursor> cursor {};
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
struct RenderBufferRef
{
    gsl::not_null<RenderBuffer const*> buffer;
    gsl::not_null<std::mutex*> guard;

    [[nodiscard]] RenderBuffer const& get() const noexcept { return *buffer; }

    RenderBufferRef(RenderBuffer const& buf, std::mutex& lock): buffer { &buf }, guard { &lock }
    {
        guard->lock();
    }

    ~RenderBufferRef() { guard->unlock(); }
};

/// Reflects the current state of a RenderDoubleBuffer object.
///
enum class RenderBufferState : uint8_t
{
    WaitingForRefresh,
    RefreshBuffersAndTrySwap,
    TrySwapBuffers
};

constexpr std::string_view to_string(RenderBufferState state) noexcept
{
    switch (state)
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
    std::array<RenderBuffer, 2> buffers {};
    std::atomic<RenderBufferState> state = RenderBufferState::WaitingForRefresh;
    std::chrono::steady_clock::time_point lastUpdate {};

    RenderBuffer& backBuffer() noexcept { return buffers[currentBackBufferIndex]; }

    RenderBufferRef frontBuffer() const
    {
        // if (state == RenderBufferState::TrySwapBuffers)
        //     const_cast<RenderDoubleBuffer*>(this)->swapBuffers(lastUpdate);
        RenderBuffer const& frontBuffer = buffers.at((currentBackBufferIndex + 1) % 2);
        return RenderBufferRef(frontBuffer, readerLock);
    }

    void clear() { backBuffer().clear(); }

    // Swaps front with back buffer. May only be invoked by the writer thread.
    bool swapBuffers(std::chrono::steady_clock::time_point now) noexcept;
};

} // namespace vtbackend
