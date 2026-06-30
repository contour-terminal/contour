// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace contour::display
{

/// GPU data-layout contracts shared between the RHI renderer (OpenGLRenderer) and the baked shaders.
///
/// These describe how vertex attributes are interleaved in the per-frame vertex buffers and how the std140
/// uniform blocks are laid out. They are the binding agreement between the C++ side (which writes the
/// buffers via QRhiResourceUpdateBatch) and the `.qsb` shaders (whose `layout(location=...)` inputs and
/// `layout(std140, binding=0)` uniform block must match exactly). A mismatch renders garbage or a black
/// terminal, so they are kept here — free of any Qt/RHI/OpenGL dependency — as named constants with a
/// single source of truth, and are covered by unit tests (RhiVertexLayout_test) that pin every offset and
/// size against the documented shader contract.
///
/// Sizes are in bytes; a "float" is 4 bytes (GLfloat / GLSL `float`).
namespace rhilayout
{
    /// Size of one 32-bit float, the unit all vertex strides/offsets are expressed in.
    inline constexpr std::uint32_t FloatSize = 4;

    /// Vertices emitted per glyph/rect tile: two triangles forming a quad.
    inline constexpr std::uint32_t VerticesPerTile = 6;

    // {{{ Background / filled-rect pass vertex layout (vec3 position + vec4 color = 7 floats).
    /// Floats per background-rect vertex (vec3 position + vec4 color).
    inline constexpr std::uint32_t RectVertexFloats = 7;
    /// Byte stride between consecutive background-rect vertices.
    inline constexpr std::uint32_t RectVertexStride = RectVertexFloats * FloatSize;
    /// Byte offset of the vec3 position attribute (location 0) within a rect vertex.
    inline constexpr std::uint32_t RectPositionOffset = 0 * FloatSize;
    /// Byte offset of the vec4 color attribute (location 1) within a rect vertex.
    inline constexpr std::uint32_t RectColorOffset = 3 * FloatSize;
    // }}}

    // {{{ Text / glyph pass vertex layout (vec3 position + vec4 texCoords + vec4 color = 11 floats).
    /// Floats per text-glyph vertex (vec3 position + vec4 texCoords + vec4 color).
    inline constexpr std::uint32_t TextVertexFloats = 3 + 4 + 4;
    /// Byte stride between consecutive text-glyph vertices.
    inline constexpr std::uint32_t TextVertexStride = TextVertexFloats * FloatSize;
    /// Byte offset of the vec3 position attribute (location 0) within a text vertex.
    inline constexpr std::uint32_t TextPositionOffset = 0 * FloatSize;
    /// Byte offset of the vec4 texCoords attribute (location 1) within a text vertex.
    inline constexpr std::uint32_t TextTexCoordOffset = 3 * FloatSize;
    /// Byte offset of the vec4 color attribute (location 2) within a text vertex.
    inline constexpr std::uint32_t TextColorOffset = 7 * FloatSize;
    // }}}

    // {{{ std140 uniform-block layouts (verified against the qsb reflection of the shader sources).
    //
    // Background UBO `Buf` { mat4 u_transform; float u_time; }:
    //   u_transform @ 0   (64 bytes), u_time @ 64 (4 bytes). Reflection extent 68; std140 rounds the block
    //   up to a multiple of 16 => 80 bytes allocated on the GPU.
    /// Byte offset of `u_transform` (mat4) in the background uniform block.
    inline constexpr std::uint32_t RectUniformTransformOffset = 0;
    /// Byte size of a mat4 (4x4 float).
    inline constexpr std::uint32_t Mat4Size = 64;
    /// Byte offset of `u_time` (float) in the background uniform block.
    inline constexpr std::uint32_t RectUniformTimeOffset = 64;
    /// Allocated size of the background uniform block (std140, multiple of 16).
    inline constexpr std::uint32_t RectUniformBlockSize = 80;

    // Text UBO `Buf` { mat4 u_transform; float u_time; float pixel_x; vec4 u_textOutlineColor; }:
    //   u_transform @ 0, u_time @ 64, pixel_x @ 68, (8 bytes pad to align the vec4), u_textOutlineColor @ 80.
    //   Block size 96 (already a multiple of 16).
    /// Byte offset of `u_transform` (mat4) in the text uniform block.
    inline constexpr std::uint32_t TextUniformTransformOffset = 0;
    /// Byte offset of `u_time` (float) in the text uniform block.
    inline constexpr std::uint32_t TextUniformTimeOffset = 64;
    /// Byte offset of `pixel_x` (float) in the text uniform block.
    inline constexpr std::uint32_t TextUniformPixelXOffset = 68;
    /// Byte offset of `u_textOutlineColor` (vec4) in the text uniform block (std140-aligned to 16).
    inline constexpr std::uint32_t TextUniformOutlineColorOffset = 80;
    /// Byte size of a vec4 (4 float).
    inline constexpr std::uint32_t Vec4Size = 16;
    /// Allocated size of the text uniform block (std140, multiple of 16).
    inline constexpr std::uint32_t TextUniformBlockSize = 96;
    // }}}
} // namespace rhilayout

} // namespace contour::display
