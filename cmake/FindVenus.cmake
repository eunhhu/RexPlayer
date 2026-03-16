# FindVenus.cmake
# Locates Mesa Venus render server for Vulkan GPU passthrough
#
# Venus is part of Mesa and provides the render server for virtio-gpu Vulkan.
# On most systems, it's built as part of Mesa's virgl/venus drivers.
#
# Sets:
#   VENUS_FOUND       - TRUE if Venus render server is found
#   VENUS_INCLUDE_DIR - Include directory
#   VENUS_LIBRARY     - Library path

find_path(VENUS_INCLUDE_DIR
    NAMES venus-protocol/vn_protocol_renderer.h
    PATHS
        /usr/include
        /usr/local/include
        /opt/homebrew/include
        ${VENUS_ROOT}/include
)

find_library(VENUS_LIBRARY
    NAMES venus-renderer venus_renderer
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /opt/homebrew/lib
        ${VENUS_ROOT}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Venus
    DEFAULT_MSG
    VENUS_LIBRARY
    VENUS_INCLUDE_DIR
)

if(VENUS_FOUND)
    if(NOT TARGET Venus::Venus)
        add_library(Venus::Venus UNKNOWN IMPORTED)
        set_target_properties(Venus::Venus PROPERTIES
            IMPORTED_LOCATION "${VENUS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${VENUS_INCLUDE_DIR}"
        )
    endif()
    message(STATUS "Venus: ${VENUS_LIBRARY}")
endif()

mark_as_advanced(VENUS_INCLUDE_DIR VENUS_LIBRARY)
