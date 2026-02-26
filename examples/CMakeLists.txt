if(LIBTERMINAL_TESTING)
    if(UNIX)
        add_executable(watch-mouse-events watch-mouse-events.cpp)
        target_link_libraries(watch-mouse-events vtbackend)

        add_executable(detect-dark-light-mode detect-dark-light-mode.cpp)

        add_executable(hud-screen-demo hud-screen-demo.cpp)
    endif()
endif()
