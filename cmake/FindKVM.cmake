# FindKVM.cmake
# Checks for KVM support on Linux

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckIncludeFile)
    check_include_file("linux/kvm.h" HAVE_KVM_H)

    if(HAVE_KVM_H AND EXISTS "/dev/kvm")
        set(KVM_FOUND TRUE)
        message(STATUS "KVM: found (/dev/kvm + linux/kvm.h)")
    else()
        set(KVM_FOUND FALSE)
        if(NOT EXISTS "/dev/kvm")
            message(STATUS "KVM: /dev/kvm not found (KVM not enabled?)")
        endif()
        if(NOT HAVE_KVM_H)
            message(STATUS "KVM: linux/kvm.h not found")
        endif()
    endif()
else()
    set(KVM_FOUND FALSE)
endif()
