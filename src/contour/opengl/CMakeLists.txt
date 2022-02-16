if(CONTOUR_BUILD_WITH_QT6)
   find_package(Qt6 COMPONENTS Core Gui OpenGLWidgets Widgets REQUIRED)
else()
   find_package(Qt5 COMPONENTS Gui Widgets REQUIRED)  # apt install qtbase5-dev libqt5gui5
endif()

CIncludeMe(
    ${CMAKE_SOURCE_DIR}/src/terminal_renderer/shared_defines.h
    "${CMAKE_CURRENT_BINARY_DIR}/shared_defines_verbatim.h"
    "shared_defines"
    "contour::opengl::verbatim"
)

set(QT_RESOURCES contour_frontend_opengl.qrc)
if(NOT(CONTOUR_BUILD_WITH_QT6))
    qt5_add_resources(QT_RESOURCES ${QT_RESOURCES})
endif()

add_library(contour_frontend_opengl
    "${CMAKE_CURRENT_BINARY_DIR}/shared_defines_verbatim.h"
    OpenGLRenderer.cpp OpenGLRenderer.h
    ShaderConfig.cpp ShaderConfig.h
    TerminalWidget.cpp TerminalWidget.h
    ${QT_RESOURCES}
)
set_target_properties(contour_frontend_opengl PROPERTIES AUTOMOC ON)
set_target_properties(contour_frontend_opengl PROPERTIES AUTORCC ON)

target_compile_definitions(contour_frontend_opengl PRIVATE CONTOUR_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

if(CONTOUR_PERF_STATS)
    target_compile_definitions(contour_frontend_opengl PRIVATE CONTOUR_PERF_STATS=1)
endif()

target_include_directories(contour_frontend_opengl PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../..")
target_link_libraries(contour_frontend_opengl terminal_renderer)
if(CONTOUR_BUILD_WITH_QT6)
   target_link_libraries(contour_frontend_opengl Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Widgets)
else()
   target_link_libraries(contour_frontend_opengl Qt5::Gui Qt5::Widgets)
endif()
set_target_properties(contour_frontend_opengl PROPERTIES AUTOMOC ON)
