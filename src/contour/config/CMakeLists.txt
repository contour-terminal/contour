# The configuration model: what contour.yml can say, how it is parsed, and how the parts of it
# the GUI owns are written back.
#
# Qt-free by construction, and the build says so: this target links no Qt at all, which is what
# lets a CONTOUR_FRONTEND_GUI=OFF build read the very configuration whose point is not to render.
# Anything here that needs a QObject, a QML property or a widget belongs a layer up.
add_library(contour_config STATIC
    Actions.cpp Actions.hpp
    AtomicFileWrite.cpp AtomicFileWrite.hpp
    Config.cpp Config.hpp
    ConfigDocumentation.hpp
    ConfigEnum.hpp
    GuiConfigStore.cpp GuiConfigStore.hpp
    LayoutBuilder.cpp LayoutBuilder.hpp
    LayoutStore.cpp LayoutStore.hpp
    ModifierNames.hpp
    TabBarMode.hpp
    UiStyle.hpp
    WindowControlStyle.cpp WindowControlStyle.hpp
)

# yaml-cpp is PUBLIC because Config.hpp exposes it in its own interface, so every consumer of the
# configuration model compiles against yaml-cpp's headers whether it names them or not.
target_link_libraries(contour_config PUBLIC
    crispy::core
    vtbackend
    vtpty
    vtrasterizer
    vtworkspace
    ${YAML_CPP_LIBRARIES}
)

target_include_directories(contour_config PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
