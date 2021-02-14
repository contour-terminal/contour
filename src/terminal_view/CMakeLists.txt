add_library(terminal_view STATIC
    TerminalView.cpp TerminalView.h
)

target_include_directories(terminal_view PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(terminal_view PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)

set(TERMINAL_VIEW_LIBRARIES terminal terminal_renderer crispy::core)

target_link_libraries(terminal_view PRIVATE ${TERMINAL_VIEW_LIBRARIES})

