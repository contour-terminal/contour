if(LIBTERMINAL_TESTING)
    if(UNIX)
        add_executable(watch-mouse-events watch-mouse-events.cpp)
        target_link_libraries(watch-mouse-events fmt::fmt-header-only terminal)
    endif()
endif()

