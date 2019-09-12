find_package(Freetype)
find_package(OpenGL)
find_package(GLEW)
find_package(glfw3 CONFIG)
find_package(glm CONFIG)

if("${CMAKE_SYSTEM}" MATCHES "Linux")
	#find_package(fontconfig)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(harfbuzz IMPORTED_TARGET harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    # installed via vcpkg
    #find_package(unofficial-fontconfig)
    find_package(harfbuzz CONFIG)
endif()

if(glfw3_FOUND AND OpenGL_FOUND AND glfw3_FOUND AND GLEW_FOUND AND glm_FOUND AND harfbuzz_FOUND)
    message(STATUS "-- Building glterm: yes")
    add_executable(glterm
        CellBackground.cpp CellBackground.h
        Flags.cpp Flags.h
        FontManager.cpp FontManager.h
        GLLogger.cpp GLLogger.h
        GLTextShaper.cpp GLTextShaper.h
        GLTerminal.cpp GLTerminal.h
        Shader.cpp Shader.h
        Window.cpp Window.h
        glterm.cpp
    )

    set(GLTERM_LIBRARIES GLEW::GLEW OpenGL::GL glm glfw Freetype::Freetype terminal)
    if("${CMAKE_SYSTEM}" MATCHES "Linux")
        list(APPEND GLTERM_LIBRARIES fontconfig)
        list(APPEND GLTERM_LIBRARIES PkgConfig::harfbuzz)
    elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
        list(APPEND GLTERM_LIBRARIES harfbuzz::harfbuzz)
        #list(APPEND GLTERM_LIBRARIES unofficial::fontconfig::fontconfig)
    endif()

    target_link_libraries(glterm PRIVATE ${GLTERM_LIBRARIES})
else()
    message(STATUS "-- Building glterm: no (missing prerequisites)")
endif()
