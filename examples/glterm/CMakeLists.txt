find_package(Freetype)
find_package(OpenGL)
find_package(glfw3 CONFIG)
find_package(GLEW)
find_package(glm CONFIG)

if("${CMAKE_SYSTEM}" MATCHES "Linux")
	#find_package(fontconfig)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    # installed via vcpkg
    find_package(unofficial-fontconfig)
endif()

if(glfw3_FOUND AND OpenGL_FOUND AND glfw3_FOUND AND GLEW_FOUND AND glm_FOUND)
    message(STATUS "-- Building glterm: yes")
    add_executable(glterm
        CellBackground.cpp CellBackground.h
        GLTerminal.cpp GLTerminal.h
        Shader.cpp Shader.h
        TextShaper.cpp TextShaper.h
        Window.h
        glterm.cpp
    )

    set(GLTERM_LIBRARIES glm glfw GLEW::GLEW OpenGL::GL Freetype::Freetype terminal)
    if("${CMAKE_SYSTEM}" MATCHES "Linux")
        list(APPEND GLTERM_LIBRARIES fontconfig)
    # elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    #     list(APPEND GLTERM_LIBRARIES unofficial::fontconfig::fontconfig)
    endif()

    target_link_libraries(glterm PRIVATE ${GLTERM_LIBRARIES})
else()
    message(STATUS "-- Building glterm: no (missing prerequisites)")
endif()
