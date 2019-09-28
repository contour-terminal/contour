find_package(Freetype REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(glm CONFIG REQUIRED)

if("${CMAKE_SYSTEM}" MATCHES "Linux" OR APPLE)
	#find_package(fontconfig)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    # installed via vcpkg
    #find_package(unofficial-fontconfig)
    find_package(harfbuzz CONFIG REQUIRED)
endif()

add_library(glterminal
    CellBackground.cpp CellBackground.h
    FontManager.cpp FontManager.h
    GLCursor.cpp GLCursor.h
    GLLogger.cpp GLLogger.h
    GLTextShaper.cpp GLTextShaper.h
    GLTerminal.cpp GLTerminal.h
    Shader.cpp Shader.h
)

target_include_directories(glterminal PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)

set(GLTERMINAL_LIBRARIES GLEW::GLEW OpenGL::GL glm Freetype::Freetype terminal)
if("${CMAKE_SYSTEM}" MATCHES "Linux" OR APPLE)
    list(APPEND GLTERMINAL_LIBRARIES fontconfig)
    list(APPEND GLTERMINAL_LIBRARIES PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    list(APPEND GLTERMINAL_LIBRARIES harfbuzz::harfbuzz)
    #list(APPEND GLTERM_LIBRARIES unofficial::fontconfig::fontconfig)
endif()

target_link_libraries(glterminal PUBLIC ${GLTERMINAL_LIBRARIES})
