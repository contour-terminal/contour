# Contour's own SSH_ASKPASS helper: the wire between the helper process ssh starts and the GUI that
# started the ssh, and the helper itself.
#
# Qt-free, like config/, command/ and cli/: the helper runs in main() before any Qt object exists, and a
# CONTOUR_FRONTEND_GUI=OFF build still answers ssh's askpass calls. The Qt half -- the QLocalServer
# broker -- lives with the frontend, in the parent's source lists.
add_library(contour_askpass STATIC
    AskpassClient.cpp AskpassClient.hpp
    AskpassProtocol.cpp AskpassProtocol.hpp
)
target_link_libraries(contour_askpass PUBLIC core::base core::async core::net)
target_include_directories(contour_askpass PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
