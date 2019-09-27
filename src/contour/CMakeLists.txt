include(FilesystemResolver)

find_package(Freetype)
find_package(OpenGL)
find_package(GLEW)
find_package(glfw3 CONFIG)
find_package(glm CONFIG)

add_executable(contour
	Contour.cpp Contour.h
	Config.cpp Config.h
	FileChangeWatcher.cpp FileChangeWatcher.h
	Flags.cpp Flags.h
	Window.cpp Window.h
	main.cpp
)

if(Boost_FILESYSTEM_FOUND)
	target_link_libraries(contour LINK_PUBLIC ${Boost_FILESYSTEM_LIBRARY})
endif()
target_link_libraries(contour PRIVATE GLEW::GLEW OpenGL::GL glm glfw glterminal yaml-cpp)
