# FindVirgl.cmake
# Locates virglrenderer library for OpenGL GPU passthrough
#
# Sets:
#   VIRGL_FOUND       - TRUE if virglrenderer is found
#   VIRGL_INCLUDE_DIR - Include directory
#   VIRGL_LIBRARY     - Library path

find_path(VIRGL_INCLUDE_DIR
    NAMES virglrenderer.h virgl/virglrenderer.h
    PATHS
        /usr/include
        /usr/local/include
        /opt/homebrew/include
        ${VIRGL_ROOT}/include
    PATH_SUFFIXES virgl
)

find_library(VIRGL_LIBRARY
    NAMES virglrenderer
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /opt/homebrew/lib
        ${VIRGL_ROOT}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Virgl
    DEFAULT_MSG
    VIRGL_LIBRARY
    VIRGL_INCLUDE_DIR
)

if(VIRGL_FOUND)
    if(NOT TARGET Virgl::Virgl)
        add_library(Virgl::Virgl UNKNOWN IMPORTED)
        set_target_properties(Virgl::Virgl PROPERTIES
            IMPORTED_LOCATION "${VIRGL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${VIRGL_INCLUDE_DIR}"
        )
    endif()
    message(STATUS "virglrenderer: ${VIRGL_LIBRARY}")
endif()

mark_as_advanced(VIRGL_INCLUDE_DIR VIRGL_LIBRARY)
