# The status-line extensions bundled with Contour, built from the crates under bundled/.
#
# Each entry is the name a configuration selects the extension by (`extensions: { <name>: {} }`), which is
# also the crate's directory and package name. It is installed as `<name>.wasm` wherever
# vtextension::bundledExtensionDirectories() looks on the platform; src/contour/CMakeLists.txt places it.
set(_bundled_extensions clock git sysload agent-status plasma)

set(_bundled_targets "")
set(_bundled_modules "")
foreach(_name IN LISTS _bundled_extensions)
    contour_add_wasm_module(NAME ${_name} CRATE "${CMAKE_CURRENT_SOURCE_DIR}/bundled/${_name}" OUTPUT_VAR _module)
    list(APPEND _bundled_targets ${_name}_wasm)
    list(APPEND _bundled_modules "${_module}")
endforeach()

# Built by default, and by any target that installs or tests them.
add_custom_target(contour_bundled_extensions ALL DEPENDS ${_bundled_targets})

# The bundled extensions' names, and their built modules in the same order, for the rules that install them.
set(CONTOUR_BUNDLED_EXTENSIONS "${_bundled_extensions}" PARENT_SCOPE)
set(CONTOUR_BUNDLED_EXTENSION_MODULES "${_bundled_modules}" PARENT_SCOPE)
