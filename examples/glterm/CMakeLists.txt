find_package(Freetype)
find_package(OpenGL)
find_package(glfw3 CONFIG)
find_package(GLEW)
find_package(glm CONFIG)

if(glfw3_FOUND AND OpenGL_FOUND AND glfw3_FOUND AND GLEW_FOUND AND glm_FOUND)
    message(STATUS "-- Building glterm: yes")
    add_executable(glterm
        CellBackground.cpp CellBackground.h
        GLTerminal.cpp GLTerminal.h
        Shader.h
        TextShaper.h
        Window.h
        glterm.cpp
    )
    target_link_libraries(glterm PRIVATE glm glfw GLEW::GLEW OpenGL::GL Freetype::Freetype terminal)
else()
    message(STATUS "-- Building glterm: no (missing prerequisites)")
endif()
