if(CONTOUR_BUILD_WITH_QT6)
   find_package(Qt6 COMPONENTS Core Gui OpenGLWidgets Widgets REQUIRED)
else()
   find_package(Qt5 COMPONENTS Gui Widgets REQUIRED)  # apt install qtbase5-dev libqt5gui5
endif()

set(QT_RESOURCES DisplayResources.qrc)
if(NOT(CONTOUR_BUILD_WITH_QT6))
    qt5_add_resources(QT_RESOURCES ${QT_RESOURCES})
endif()

add_library(ContourTerminalDisplay STATIC
    Blur.cpp Blur.h
    OpenGLRenderer.cpp OpenGLRenderer.h
    ShaderConfig.cpp ShaderConfig.h
    TerminalWidget.cpp TerminalWidget.h
    ${QT_RESOURCES}
)
set_target_properties(ContourTerminalDisplay PROPERTIES AUTOMOC ON)
set_target_properties(ContourTerminalDisplay PROPERTIES AUTORCC ON)

# Disable all deprecated Qt functions prior to Qt 6.0
target_compile_definitions(ContourTerminalDisplay PRIVATE QT_DISABLE_DEPRECATED_BEFORE=0x050F00)

target_compile_definitions(ContourTerminalDisplay PRIVATE CONTOUR_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

if(CONTOUR_PERF_STATS)
    target_compile_definitions(ContourTerminalDisplay PRIVATE CONTOUR_PERF_STATS=1)
endif()

target_include_directories(ContourTerminalDisplay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../..")
target_link_libraries(ContourTerminalDisplay vtrasterizer)
if(CONTOUR_BUILD_WITH_QT6)
    target_link_libraries(ContourTerminalDisplay Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Widgets)
else()
    target_link_libraries(ContourTerminalDisplay Qt5::Gui Qt5::Widgets)
endif()
set_target_properties(ContourTerminalDisplay PROPERTIES AUTOMOC ON)
