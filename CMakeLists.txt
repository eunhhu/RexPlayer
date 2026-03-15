cmake_minimum_required(VERSION 3.25)
project(RexPlayer
    VERSION 0.1.0
    DESCRIPTION "Lightweight high-performance Android app player"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---
option(REX_ENABLE_KVM   "Enable KVM hypervisor backend (Linux)"   OFF)
option(REX_ENABLE_WHPX  "Enable WHPX hypervisor backend (Windows)" OFF)
option(REX_ENABLE_HVF   "Enable HVF hypervisor backend (macOS)"   OFF)
option(REX_ENABLE_TESTS "Build tests"                              ON)
option(REX_ENABLE_GPU   "Enable GPU acceleration"                  OFF)
option(REX_ENABLE_GUI   "Build Qt GUI"                             ON)

# Auto-detect hypervisor based on host OS
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(REX_ENABLE_KVM ON CACHE BOOL "" FORCE)
    set(REX_PLATFORM "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(REX_ENABLE_WHPX ON CACHE BOOL "" FORCE)
    set(REX_PLATFORM "windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(REX_ENABLE_HVF ON CACHE BOOL "" FORCE)
    set(REX_PLATFORM "macos")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

message(STATUS "RexPlayer v${PROJECT_VERSION}")
message(STATUS "Platform: ${REX_PLATFORM}")
message(STATUS "KVM: ${REX_ENABLE_KVM}, WHPX: ${REX_ENABLE_WHPX}, HVF: ${REX_ENABLE_HVF}")

# --- CMake module path ---
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# --- Corrosion (Rust/Cargo integration) ---
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5
)
FetchContent_MakeAvailable(Corrosion)

# --- Rust middleware workspace ---
corrosion_import_crate(MANIFEST_PATH middleware/Cargo.toml)

# --- HAL library ---
add_library(rex_hal STATIC
    src/hal/hypervisor_factory.cpp
)
target_include_directories(rex_hal PUBLIC src/hal/include)
target_compile_definitions(rex_hal PRIVATE
    $<$<BOOL:${REX_ENABLE_KVM}>:REX_HAS_KVM>
    $<$<BOOL:${REX_ENABLE_WHPX}>:REX_HAS_WHPX>
    $<$<BOOL:${REX_ENABLE_HVF}>:REX_HAS_HVF>
)

if(REX_ENABLE_KVM)
    target_sources(rex_hal PRIVATE src/hal/kvm/kvm_hypervisor.cpp)
endif()
if(REX_ENABLE_WHPX)
    target_sources(rex_hal PRIVATE src/hal/whpx/whpx_hypervisor.cpp)
endif()
if(REX_ENABLE_HVF)
    target_sources(rex_hal PRIVATE src/hal/hvf/hvf_hypervisor.cpp)
    target_link_libraries(rex_hal PRIVATE "-framework Hypervisor")
endif()

# --- VMM Core library ---
add_library(rex_vmm STATIC
    src/vmm/vm.cpp
    src/vmm/memory_manager.cpp
    src/vmm/device_manager.cpp
    src/vmm/boot_x86.cpp
    src/vmm/boot_arm64.cpp
    src/vmm/snapshot.cpp
)
target_include_directories(rex_vmm PUBLIC src/vmm/include)
target_link_libraries(rex_vmm PUBLIC rex_hal)

# --- Legacy devices library ---
add_library(rex_devices_legacy STATIC
    src/devices/legacy/uart_16550.cpp
    src/devices/legacy/i8042.cpp
    src/devices/legacy/rtc.cpp
    src/devices/legacy/pci_host.cpp
)
target_include_directories(rex_devices_legacy PUBLIC src/devices)
target_link_libraries(rex_devices_legacy PUBLIC rex_vmm)

# --- GPU library ---
add_library(rex_gpu STATIC
    src/gpu/display.cpp
)
target_include_directories(rex_gpu PUBLIC src/gpu)

# --- Platform abstraction ---
add_library(rex_platform STATIC
    src/platform/platform_${REX_PLATFORM}.cpp
)
target_include_directories(rex_platform PUBLIC src/platform)

# --- Qt GUI ---
if(REX_ENABLE_GUI)
    find_package(Qt6 COMPONENTS Widgets Gui Core QUIET)
    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)

        add_executable(rexplayer
            src/gui/main.cpp
            src/gui/mainwindow.cpp
            src/gui/display_widget.cpp
            src/gui/input_handler.cpp
            src/gui/settings_dialog.cpp
        )
        target_link_libraries(rexplayer PRIVATE
            rex_vmm
            rex_devices_legacy
            rex_gpu
            rex_platform
            Qt6::Widgets
            Qt6::Gui
            Qt6::Core
        )
        target_include_directories(rexplayer PRIVATE src/gui)

        # macOS: create app bundle
        if(APPLE)
            set_target_properties(rexplayer PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_BUNDLE_NAME "RexPlayer"
                MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            )
        endif()

        message(STATUS "Qt GUI: enabled (Qt ${Qt6_VERSION})")
    else()
        message(STATUS "Qt GUI: disabled (Qt6 not found)")
    endif()
endif()

# --- Tests ---
if(REX_ENABLE_TESTS)
    enable_testing()

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()
