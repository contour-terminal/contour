include(FilesystemResolver)

find_package(Freetype)
find_package(OpenGL)
find_package(GLEW)
find_package(glm CONFIG)

set(GLFW_VER 3.2)
if("${CMAKE_SYSTEM}" MATCHES "Windows")
	set(GLFW_VER 3.3)
endif()

find_package(glfw3 ${GLFW_VER} CONFIG REQUIRED)

add_executable(contour
	Contour.cpp Contour.h
	Config.cpp Config.h
	Flags.cpp Flags.h
	Window.cpp Window.h
	main.cpp
)

set(contour_LIBRARIES GLEW::GLEW OpenGL::GL glm glfw terminal_view ground yaml-cpp)
if(Boost_FILESYSTEM_FOUND)
    list(APPEND contour_LIBRARIES ${Boost_FILESYSTEM_LIBRARY})
endif()

target_link_libraries(contour PRIVATE ${contour_LIBRARIES})
