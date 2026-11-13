// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/session/InProcessDirectory.hpp>
#include <contour/window/WorkspaceDirectorySource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>

TEST_CASE("DaemonLink and InProcessDirectory both satisfy the interface", "[directory][inprocess]")
{
    static_assert(std::derived_from<contour::remote::DaemonLink, contour::window::IWorkspaceDirectorySource>);
    static_assert(
        std::derived_from<contour::session::InProcessDirectory, contour::window::IWorkspaceDirectorySource>);
    SUCCEED();
}
