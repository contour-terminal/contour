add_executable(chrono chrono.cpp)
target_link_libraries(chrono PUBLIC crispy::core text_shaper)

if(CONTOUR_INSTALL_TOOLS)
    if(WIN32)
        install(TARGETS chrono DESTINATION bin)
    elseif(APPLE)
        # See: https://stackoverflow.com/questions/35612687/cmake-macos-x-bundle-with-bundleutiliies-for-qt-application/48035834#48035834
        set(APP_NAME contour)
        set(App_Contents "${APP_NAME}.app/Contents")
        set(INSTALL_RUNTIME_DIR "${App_Contents}/MacOS")
        set(INSTALL_CMAKE_DIR "${App_Contents}/Resources")

        set(INSTALL_RUNTIME_DIR "${App_Contents}/MacOS")
        install(TARGETS chrono
            BUNDLE DESTINATION . COMPONENT Runtime
            RUNTIME DESTINATION "${INSTALL_RUNTIME_DIR}" COMPONENT Runtime
        )
    else()
        install(TARGETS chrono DESTINATION bin)
    endif()
endif()
