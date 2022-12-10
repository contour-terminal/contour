if(("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU") OR ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    #(TODO: first get rid of these warnings) add_compile_options(-Werror)
endif()

include(PedanticCompiler)

add_subdirectory(crispy)
add_subdirectory(text_shaper)
add_subdirectory(vtpty)
add_subdirectory(vtparser)
add_subdirectory(vtbackend)
add_subdirectory(vtrasterizer)

if(CONTOUR_FRONTEND_GUI)
    add_subdirectory(contour)
endif()
