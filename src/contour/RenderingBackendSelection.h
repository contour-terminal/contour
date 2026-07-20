// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>

#include <cstdint>

namespace contour
{

/// The host platform, as far as RHI-backend availability is concerned.
///
/// Modeled as data (rather than being hard-wired to `#if defined(_WIN32)` at the single call site) so the
/// availability rule below is a pure function of (platform, backend) and can be unit-tested for every OS
/// on any host — the availability matrix is otherwise invisible to the tests that run on one platform.
enum class RhiPlatform : uint8_t
{
    Windows,
    MacOS,
    Other, ///< Linux, BSD, and anything else Qt runs on with an OpenGL/Vulkan RHI.
};

/// The platform this binary was compiled for.
/// @return The compile-time @ref RhiPlatform.
[[nodiscard]] constexpr RhiPlatform currentRhiPlatform() noexcept
{
#ifdef _WIN32
    return RhiPlatform::Windows;
#elifdef __APPLE__
    return RhiPlatform::MacOS;
#else
    return RhiPlatform::Other;
#endif
}

/// Decides whether @p backend can actually drive a visible window on @p platform.
///
/// A backend the platform cannot provide would abort at RHI initialization; worse, a backend the platform
/// *nominally* provides but cannot composite (desktop OpenGL on macOS — Apple-deprecated, and Qt's
/// RHI-over-OpenGL path brings the window up but never composites the scene graph to the NSView, leaving a
/// correctly-sized yet invisible window) hangs the user with no error. Both are reported as unavailable so
/// the caller can fall back to @c Auto (which resolves to the platform-native backend) rather than start
/// into a dead window.
///
/// @c Auto and @c Software are always available: @c Auto lets Qt pick, and @c Software takes the
/// software-OpenGL attribute path.
///
/// @param platform The host platform.
/// @param backend  The configured renderer backend.
/// @return @c true when @p backend is usable on @p platform.
[[nodiscard]] constexpr bool isRenderingBackendAvailable(RhiPlatform platform,
                                                         config::RenderingBackend backend) noexcept
{
    using config::RenderingBackend;
    switch (platform)
    {
        case RhiPlatform::Windows:
            // Metal is Apple-only; everything else (D3D11/12, Vulkan, OpenGL) is fine.
            return backend != RenderingBackend::Metal;
        case RhiPlatform::MacOS:
            // Metal only. Direct3D and Vulkan are absent; OpenGL is present but does not composite.
            return backend != RenderingBackend::Direct3D11 && backend != RenderingBackend::Direct3D12
                   && backend != RenderingBackend::Vulkan && backend != RenderingBackend::OpenGL;
        case RhiPlatform::Other:
            // OpenGL/Vulkan territory; Direct3D and Metal are absent.
            return backend != RenderingBackend::Direct3D11 && backend != RenderingBackend::Direct3D12
                   && backend != RenderingBackend::Metal;
    }
    return false;
}

/// Resolves the configured backend to the backend actually used on @p platform.
///
/// The identity when @ref isRenderingBackendAvailable, otherwise @c Auto (let Qt choose the native
/// backend). This is the single decision the startup path applies before creating the first
/// @c QQuickWindow; keeping it pure lets the fallback be asserted directly, without a live RHI.
///
/// @param platform The host platform.
/// @param backend  The configured renderer backend.
/// @return The backend to use — @p backend, or @c Auto when @p backend is unavailable.
[[nodiscard]] constexpr config::RenderingBackend resolveRenderingBackend(
    RhiPlatform platform, config::RenderingBackend backend) noexcept
{
    return isRenderingBackendAvailable(platform, backend) ? backend : config::RenderingBackend::Auto;
}

} // namespace contour
