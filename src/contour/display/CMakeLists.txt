find_package(Qt6 COMPONENTS Core Gui GuiPrivate Quick QuickControls2 Multimedia OpenGL REQUIRED)
find_package(Qt6 COMPONENTS ShaderTools REQUIRED)

set(QT_RESOURCES DisplayResources.qrc)

add_library(ContourTerminalDisplay STATIC
    Blur.cpp Blur.h
    OpenGLRenderer.cpp OpenGLRenderer.h
    ShaderConfig.cpp ShaderConfig.h
    TerminalDisplay.cpp TerminalDisplay.h
    TerminalRenderNode.cpp TerminalRenderNode.h
    ${QT_RESOURCES}
)
set_target_properties(ContourTerminalDisplay PROPERTIES AUTOMOC ON)
set_target_properties(ContourTerminalDisplay PROPERTIES AUTORCC ON)

# Bake the node-path shaders (background + text pipelines) into Qt RHI .qsb blobs.
# These are embedded as resources at ":/contour/display/shaders/<name>.qsb" and loaded
# at runtime via QShader::fromSerialized() by the RHI render path.
qt6_add_shaders(ContourTerminalDisplay "contour_rhi_shaders"
    PRECOMPILE
    OPTIMIZED
    PREFIX
        "/contour/display/shaders"
    BASE
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    FILES
        shaders/background.vert
        shaders/background.frag
        shaders/text.vert
        shaders/text.frag
)

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
# Note: qt6_add_shaders() links Qt6::ShaderTools via the keyword signature, so every
# target_link_libraries() call on this target must also use the keyword signature.
target_link_libraries(ContourTerminalDisplay PUBLIC vtrasterizer)
target_link_libraries(ContourTerminalDisplay PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate # rhi/qrhi.h is a private header (RHI resource model in OpenGLRenderer)
    Qt6::Multimedia
    Qt6::OpenGL
    Qt6::Quick
    Qt6::QuickControls2
)
set_target_properties(ContourTerminalDisplay PROPERTIES AUTOMOC ON)
