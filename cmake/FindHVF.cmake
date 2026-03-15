# FindHVF.cmake
# Checks for Hypervisor.framework on macOS

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    include(CheckIncludeFile)
    set(CMAKE_REQUIRED_FLAGS "-framework Hypervisor")
    check_include_file("Hypervisor/hv.h" HAVE_HV_H)

    if(HAVE_HV_H)
        set(HVF_FOUND TRUE)
        set(HVF_LIBRARIES "-framework Hypervisor")
        message(STATUS "HVF: found (Hypervisor.framework)")
    else()
        set(HVF_FOUND FALSE)
        message(STATUS "HVF: Hypervisor.framework not found")
    endif()
else()
    set(HVF_FOUND FALSE)
endif()
