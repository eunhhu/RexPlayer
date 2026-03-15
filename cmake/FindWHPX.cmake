# FindWHPX.cmake
# Checks for Windows Hypervisor Platform (WHPX) on Windows

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    include(CheckIncludeFile)
    check_include_file("WinHvPlatform.h" HAVE_WHPX_H)

    if(HAVE_WHPX_H)
        set(WHPX_FOUND TRUE)
        set(WHPX_LIBRARIES "WinHvPlatform" "WinHvEmulation")
        message(STATUS "WHPX: found (WinHvPlatform.h)")
    else()
        set(WHPX_FOUND FALSE)
        message(STATUS "WHPX: WinHvPlatform.h not found")
    endif()
else()
    set(WHPX_FOUND FALSE)
endif()
