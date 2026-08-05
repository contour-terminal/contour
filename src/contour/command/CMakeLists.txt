# The command vocabulary: every action the user can invoke by name, the catalog the command
# palette ranks, the recently-used list behind it, and the context menus assembled from the same
# rows.
#
# Qt-free, deliberately: what a menu row *is* and whether it should be shown are decisions about
# terminal state, not about Qt, so they are decided here and rendered a layer up. That split is
# what makes ContextMenu_test and CommandPaletteModel's ranking testable without a display.
add_library(contour_command STATIC
    Command.cpp Command.h
    CommandCatalog.cpp CommandCatalog.h
    CommandHistory.cpp CommandHistory.h
    CommandHistoryStore.cpp CommandHistoryStore.h
    ContextMenu.cpp ContextMenu.h
    ContextMenuTable.h
    FuzzyFilter.cpp FuzzyFilter.h
    Shortcut.cpp Shortcut.h
    TitleBarContextMenu.cpp TitleBarContextMenu.h
)

target_link_libraries(contour_command PUBLIC contour_config)

target_include_directories(contour_command PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
