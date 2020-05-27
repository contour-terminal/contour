include(${CMAKE_CURRENT_LIST_DIR}/../FilesystemResolver.cmake)
add_executable(cincludeme cincludeme.cpp)

if(USING_BOOST)
    target_link_libraries(cincludeme Boost::filesystem)
endif()
