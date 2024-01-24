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
