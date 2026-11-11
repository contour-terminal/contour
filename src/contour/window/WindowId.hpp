// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <boxed-cpp/boxed.hpp>

namespace contour::window
{

namespace detail::tags
{
    struct WindowId
    {
    };
} // namespace detail::tags

/// Names one OS window of the application, as the session manager minted it. Unlike a
/// vtworkspace::WorkspaceId it outlives the workspace the window happens to display: a window may own
/// several workspaces and switch among them, and keeps its id throughout.
using WindowId = boxed::boxed<uint64_t, detail::tags::WindowId>;

} // namespace contour::window
