find_package(Freetype REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(glm CONFIG REQUIRED)

if(APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(fontconfig REQUIRED IMPORTED_TARGET fontconfig)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux")
	  #find_package(fontconfig)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    # installed via vcpkg
    #find_package(unofficial-fontconfig)
    find_package(harfbuzz CONFIG REQUIRED)
endif()

add_library(terminal_view
    CellBackground.cpp CellBackground.h
    FontManager.cpp FontManager.h
    GLCursor.cpp GLCursor.h
    GLLogger.cpp GLLogger.h
    GLTextShaper.cpp GLTextShaper.h
    Selector.cpp Selector.h
    Shader.cpp Shader.h
    TerminalView.cpp TerminalView.h
)

target_include_directories(terminal_view PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)

set(TERMINAL_VIEW_LIBRARIES GLEW::GLEW OpenGL::GL glm Freetype::Freetype terminal)
if(APPLE)
    list(APPEND TERMINAL_VIEW_LIBRARIES PkgConfig::fontconfig)
    list(APPEND TERMINAL_VIEW_LIBRARIES PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux" OR APPLE)
    list(APPEND TERMINAL_VIEW_LIBRARIES fontconfig)
    list(APPEND TERMINAL_VIEW_LIBRARIES PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    list(APPEND TERMINAL_VIEW_LIBRARIES harfbuzz::harfbuzz)
    #list(APPEND GLTERM_LIBRARIES unofficial::fontconfig::fontconfig)
endif()

target_link_libraries(terminal_view PUBLIC ${TERMINAL_VIEW_LIBRARIES})
