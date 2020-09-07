include(FilesystemResolver)

find_package(Qt5 COMPONENTS Gui Network Widgets REQUIRED)  # apt install qtbase5-dev libqt5gui5
find_package(OpenGL REQUIRED)
find_package(Freetype REQUIRED)

option(CONTOUR_BLUR_PLATFORM_KWIN "Enables support for blurring transparent background when using KWin (KDE window manager)." OFF)
option(CONTOUR_PERF_STATS "Enables debug printing some performance stats." OFF)
option(CONTOUR_VT_METRICS "Enables collecting and exit-printing some VT usage metrics." OFF)

# /// {{{ Linux/KDE
# ! apt install extra-cmake-modules libkf5windowsystem-dev
if(CONTOUR_BLUR_PLATFORM_KWIN)
    set(KF5_MIN_VERSION "5.12.0")
    find_package(ECM ${KF5_MIN_VERSION} REQUIRED NO_MODULE)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH})
    include(FeatureSummary)
    find_package(KF5 ${KF5_MIN_VERSION} REQUIRED WindowSystem)
endif()
# }}}

CIncludeMe(contour.yml "${CMAKE_CURRENT_BINARY_DIR}/contour_yaml.h" "default_config_yaml" "contour")

set(contour_SRCS
    Actions.cpp Actions.h
    Config.cpp Config.h
    Controller.cpp Controller.h
    DebuggerService.cpp DebuggerService.h
    FileChangeWatcher.cpp FileChangeWatcher.h
    LoggingSink.cpp LoggingSink.h
    TerminalWindow.cpp TerminalWindow.h
    main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/contour_yaml.h"
)

add_executable(contour ${contour_SRCS})
target_include_directories(contour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(contour PRIVATE
    CONTOUR_VERSION_MAJOR=${CMAKE_PROJECT_VERSION_MAJOR}
    CONTOUR_VERSION_MINOR=${CMAKE_PROJECT_VERSION_MINOR}
    CONTOUR_VERSION_PATCH=${CMAKE_PROJECT_VERSION_PATCH}
    CONTOUR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
set_target_properties(contour PROPERTIES AUTOMOC ON)

if(CONTOUR_PERF_STATS)
    target_compile_definitions(contour PRIVATE CONTOUR_PERF_STATS)
endif()

if(CONTOUR_VT_METRICS)
    target_compile_definitions(contour PRIVATE CONTOUR_VT_METRICS)
endif()

if(WIN32)
    if (NOT ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug"))
        set_target_properties(contour PROPERTIES
            # Tell MSVC to create a GUI application.
            WIN32_EXECUTABLE ON

            # Tell MSVC to use main instead of WinMain for Windows subsystem executables.
            LINK_FLAGS "/ENTRY:mainCRTStartup"
        )
    endif()
elseif(APPLE)
    set(contour_version "0.1.0_pre")
    set_target_properties(contour PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Contour Terminal"
        MACOSX_BUNDLE_INFO_STRING "Contour Terminal Emulator"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.christianparpart.contour"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${contour_version}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${contour_version}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${contour_version}"
        # TODO: MACOSX_BUNDLE_ICON_FILE "contour.icns"
        # TODO: RESOURCE "images/icon.icns"
    )
endif()

set(contour_LIBRARIES terminal_view yaml-cpp Qt5::Gui Qt5::Widgets Qt5::Network OpenGL::GL)
if(Boost_FILESYSTEM_FOUND)
    target_include_directories(contour PRIVATE ${Boost_INCLUDE_DIRS})
    list(APPEND contour_LIBRARIES ${Boost_LIBRARIES})
endif()
if(CONTOUR_BLUR_PLATFORM_KWIN) # TODO: auto-enable on Linux, if KDE dev libs were found.
    list(APPEND contour_LIBRARIES KF5::WindowSystem)
	target_compile_definitions(contour PRIVATE CONTOUR_BLUR_PLATFORM_KWIN)
endif()

if(CONTOUR_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address)
    #add_compile_options(-fsanitize=pointer-compare) # cannot be enabled with -fsanitize=thread
    #add_compile_options(-fsanitize=thread)
    add_compile_options(-fsanitize=undefined)
    target_link_libraries(contour PRIVATE ubsan)
endif()

target_link_libraries(contour PRIVATE ${contour_LIBRARIES})

install(TARGETS contour DESTINATION bin)
