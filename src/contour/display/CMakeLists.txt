find_package(Qt6 COMPONENTS Core Gui Quick QuickControls2 Multimedia OpenGL REQUIRED)

set(QT_RESOURCES DisplayResources.qrc)

add_library(ContourTerminalDisplay STATIC
    Blur.cpp Blur.h
    OpenGLRenderer.cpp OpenGLRenderer.h
    ShaderConfig.cpp ShaderConfig.h
    TerminalDisplay.cpp TerminalDisplay.h
    ${QT_RESOURCES}
)
set_target_properties(ContourTerminalDisplay PROPERTIES AUTOMOC ON)
set_target_properties(ContourTerminalDisplay PROPERTIES AUTORCC ON)

# Disable all deprecated Qt functions prior to Qt 6.0
target_compile_definitions(ContourTerminalDisplay PRIVATE QT_DISABLE_DEPRECATED_BEFORE=0x050F00)

target_compile_definitions(ContourTerminalDisplay PRIVATE CONTOUR_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

target_compile_definitions(ContourTerminalDisplay PRIVATE CONTOUR_APP_ID="${AppId}")

option(CONTOUR_DEBUG_OPENGL "Enables OpenGL debugging." OFF)
if(CONTOUR_DEBUG_OPENGL)
    target_compile_definitions(ContourTerminalDisplay PRIVATE Q_ENABLE_OPENGL_FUNCTIONS_DEBUG)
endif()

if(CONTOUR_PERF_STATS)
    target_compile_definitions(ContourTerminalDisplay PRIVATE CONTOUR_PERF_STATS=1)
endif()

target_include_directories(ContourTerminalDisplay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../..")
target_link_libraries(ContourTerminalDisplay vtrasterizer)
target_link_libraries(ContourTerminalDisplay Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Multimedia Qt6::Quick Qt6::QuickControls2)
set_target_properties(ContourTerminalDisplay PROPERTIES AUTOMOC ON)
