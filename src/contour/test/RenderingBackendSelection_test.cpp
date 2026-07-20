// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure RHI-backend selection rule (RenderingBackendSelection.h): the per-platform
// availability matrix and the Auto fallback the startup path applies before creating the first
// QQuickWindow. These assert the whole (platform x backend) matrix on any host — including the macOS
// rule that rejects desktop OpenGL, whose RHI path maps a window but never composites it.

#include <contour/RenderingBackendSelection.h>

#include <catch2/catch_test_macros.hpp>

using contour::currentRhiPlatform;
using contour::isRenderingBackendAvailable;
using contour::resolveRenderingBackend;
using contour::RhiPlatform;
using RB = contour::config::RenderingBackend;

TEST_CASE("Auto and Software are available on every platform", "[rendering-backend]")
{
    for (auto const platform: { RhiPlatform::Windows, RhiPlatform::MacOS, RhiPlatform::Other })
    {
        CHECK(isRenderingBackendAvailable(platform, RB::Auto));
        CHECK(isRenderingBackendAvailable(platform, RB::Software));
        // Auto/Software never trigger the fallback: they resolve to themselves.
        CHECK(resolveRenderingBackend(platform, RB::Auto) == RB::Auto);
        CHECK(resolveRenderingBackend(platform, RB::Software) == RB::Software);
    }
}

TEST_CASE("macOS accepts only Metal among the GPU backends", "[rendering-backend]")
{
    CHECK(isRenderingBackendAvailable(RhiPlatform::MacOS, RB::Metal));
    // Desktop OpenGL brings up a window that never composites -> treated as unavailable.
    CHECK_FALSE(isRenderingBackendAvailable(RhiPlatform::MacOS, RB::OpenGL));
    CHECK_FALSE(isRenderingBackendAvailable(RhiPlatform::MacOS, RB::Vulkan));
    CHECK_FALSE(isRenderingBackendAvailable(RhiPlatform::MacOS, RB::Direct3D11));
    CHECK_FALSE(isRenderingBackendAvailable(RhiPlatform::MacOS, RB::Direct3D12));

    // The reported regression: a stale `backend: OpenGL` config self-heals to Auto (-> Metal) on macOS.
    CHECK(resolveRenderingBackend(RhiPlatform::MacOS, RB::OpenGL) == RB::Auto);
    CHECK(resolveRenderingBackend(RhiPlatform::MacOS, RB::Metal) == RB::Metal);
}

TEST_CASE("Windows accepts every backend except Metal", "[rendering-backend]")
{
    CHECK(isRenderingBackendAvailable(RhiPlatform::Windows, RB::OpenGL));
    CHECK(isRenderingBackendAvailable(RhiPlatform::Windows, RB::Vulkan));
    CHECK(isRenderingBackendAvailable(RhiPlatform::Windows, RB::Direct3D11));
    CHECK(isRenderingBackendAvailable(RhiPlatform::Windows, RB::Direct3D12));
    CHECK_FALSE(isRenderingBackendAvailable(RhiPlatform::Windows, RB::Metal));

    CHECK(resolveRenderingBackend(RhiPlatform::Windows, RB::OpenGL) == RB::OpenGL);
    CHECK(resolveRenderingBackend(RhiPlatform::Windows, RB::Metal) == RB::Auto);
}

TEST_CASE("Other (Linux/BSD) accepts OpenGL and Vulkan, rejects Metal and Direct3D", "[rendering-backend]")
{
    CHECK(isRenderingBackendAvailable(RhiPlatform::Other, RB::OpenGL));
    CHECK(isRenderingBackendAvailable(RhiPlatform::Other, RB::Vulkan));
    CHECK_FALSE(isRenderingBackendAvailable(RhiPlatform::Other, RB::Metal));
    CHECK_FALSE(isRenderingBackendAvailable(RhiPlatform::Other, RB::Direct3D11));
    CHECK_FALSE(isRenderingBackendAvailable(RhiPlatform::Other, RB::Direct3D12));

    CHECK(resolveRenderingBackend(RhiPlatform::Other, RB::OpenGL) == RB::OpenGL);
    CHECK(resolveRenderingBackend(RhiPlatform::Other, RB::Metal) == RB::Auto);
}

TEST_CASE("currentRhiPlatform matches the host this test was built for", "[rendering-backend]")
{
#ifdef _WIN32
    CHECK(currentRhiPlatform() == RhiPlatform::Windows);
#elifdef __APPLE__
    CHECK(currentRhiPlatform() == RhiPlatform::MacOS);
#else
    CHECK(currentRhiPlatform() == RhiPlatform::Other);
#endif
}
