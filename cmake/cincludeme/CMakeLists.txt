include("${CMAKE_CURRENT_LIST_DIR}/../FilesystemResolver.cmake")
add_executable(cincludeme cincludeme.cpp)

if(${USING_BOOST_FILESYSTEM})
    target_compile_definitions(cincludeme PRIVATE USING_BOOST_FILESYSTEM=1)
endif()

target_link_libraries(cincludeme "${FILESYSTEM_LIBS}")
