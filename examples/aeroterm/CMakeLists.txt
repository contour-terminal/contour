find_package(Freetype)
find_package(OpenGL)
find_package(GLEW)
find_package(glfw3 CONFIG)
find_package(glm CONFIG)

if(LIBTERMINAL_GLTERMINAL AND glfw3_FOUND)
    message(STATUS "-- Building Aero Terminal Emulator: yes")
    add_executable(aeroterm
        AeroTerminal.cpp AeroTerminal.h
        Flags.cpp Flags.h
        Window.cpp Window.h
        main.cpp
    )
    target_link_libraries(aeroterm PRIVATE GLEW::GLEW OpenGL::GL glm glfw glterminal)
else()
    message(STATUS "-- Building Aero Terminal: no (missing prerequisites)")
endif()
