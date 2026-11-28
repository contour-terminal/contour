// SPDX-License-Identifier: Apache-2.0
//
// The close confirmation's wording is one table row per kind, so each row is asserted whole.

#include <contour/window/CloseDialogTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace contour::window;

TEST_CASE("CloseDialogTable.a workspace names its tabs, panes and running program", "[closedialog]")
{
    auto const text = closeDialogText(
        CloseKind::Workspace,
        { .label = "billing", .tabs = 2, .panes = 3, .program = "make deploy", .daemon = "" });
    CHECK(text.heading == "Close the workspace “billing”?");
    CHECK(text.message == "This ends 2 tabs and 3 panes. “make deploy” is still running.");
    CHECK(text.acceptText == "Close Workspace");
}

TEST_CASE("CloseDialogTable.a tab names its panes", "[closedialog]")
{
    auto const text = closeDialogText(
        CloseKind::Tab, { .label = "claude", .tabs = 1, .panes = 1, .program = "", .daemon = "" });
    CHECK(text.heading == "Close the tab “claude”?");
    CHECK(text.message == "This ends the tab and its 1 pane.");
    CHECK(text.acceptText == "Close Tab");
}

TEST_CASE("CloseDialogTable.a pane says what runs in it, and an unnamed node has a plain title",
          "[closedialog]")
{
    auto const text = closeDialogText(CloseKind::Pane,
                                      { .label = "", .tabs = 0, .panes = 0, .program = "vim", .daemon = "" });
    CHECK(text.heading == "Close this pane?");
    CHECK(text.message == "This ends the pane and its shell. “vim” is still running.");
    CHECK(text.acceptText == "Close Pane");
    CHECK(closeDialogText(CloseKind::Workspace, CloseSubject {}).heading == "Close this workspace?");
}

TEST_CASE("CloseDialogTable.every kind has a destructive label of its own", "[closedialog]")
{
    for (auto const& row: CloseDialogRows)
        CHECK_FALSE(row.destructive.empty());
}

TEST_CASE("CloseDialogTable.a close on another daemon names it", "[closedialog]")
{
    // What closes is not the window's: the message says where it runs, last.
    auto const text = closeDialogText(
        CloseKind::Tab, { .label = "build", .tabs = 1, .panes = 2, .program = "make", .daemon = "starfive" });
    CHECK(text.heading == "Close the tab “build”?");
    CHECK(text.message == "This ends the tab and its 2 panes. “make” is still running. It runs on starfive.");
    for (auto const kind: { CloseKind::Workspace, CloseKind::Tab, CloseKind::Pane })
        CHECK(
            closeDialogText(kind, { .label = "", .tabs = 1, .panes = 1, .program = "", .daemon = "starfive" })
                .message.ends_with("It runs on starfive."));
}

TEST_CASE("CloseDialogTable.an own close says nothing of a daemon", "[closedialog]")
{
    for (auto const kind: { CloseKind::Workspace, CloseKind::Tab, CloseKind::Pane })
        CHECK_FALSE(
            closeDialogText(kind, { .label = "x", .tabs = 1, .panes = 1, .program = "vim", .daemon = "" })
                .message.contains("runs on"));
}
