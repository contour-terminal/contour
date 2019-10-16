include(FilesystemResolver)

find_package(Freetype REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(glm CONFIG REQUIRED)

set(GLFW_VER 3.2)
if("${CMAKE_SYSTEM}" MATCHES "Windows")
	set(GLFW_VER 3.3)
endif()

find_package(glfw3 ${GLFW_VER} CONFIG REQUIRED)

option(CONTOUR_BLUR_PLATFORM_KWIN_X11 "Enables support for blurring transparent background when using KWin (KDE window manager)." OFF)

add_executable(contour
	Contour.cpp Contour.h
	Config.cpp Config.h
	Flags.cpp Flags.h
	UIWindow.cpp UIWindow.h
	main.cpp
)

set(contour_LIBRARIES GLEW::GLEW OpenGL::GL glm glfw terminal_view ground yaml-cpp)
if(Boost_FILESYSTEM_FOUND)
    list(APPEND contour_LIBRARIES ${Boost_FILESYSTEM_LIBRARY})
endif()
if(CONTOUR_BLUR_PLATFORM_KWIN_X11)
	find_package(X11 REQUIRED)
	list(APPEND contour_LIBRARIES ${X11_LIBRARIES})
	target_compile_definitions(contour PRIVATE CONTOUR_BLUR_PLATFORM_KWIN_X11)
endif()

target_link_libraries(contour PRIVATE ${contour_LIBRARIES})
