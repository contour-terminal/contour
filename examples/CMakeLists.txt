if(LIBTERMINAL_TESTING)
    if(UNIX)
        add_executable(watch-mouse-events watch-mouse-events.cpp)
        target_link_libraries(watch-mouse-events vtbackend)

        add_executable(detect-dark-light-mode detect-dark-light-mode.cpp)

        add_executable(watch-dec-locator watch-dec-locator.cpp)
        add_executable(drcs-demo drcs-demo.cpp)

        add_executable(multi-page-demo multi-page-demo.cpp)
    endif()
endif()
