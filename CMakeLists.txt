cmake_minimum_required(VERSION 3.25)
project(RexPlayer
    VERSION 0.2.0
    DESCRIPTION "Lightweight high-performance Android app player"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(REX_ENABLE_TESTS "Build tests" ON)

# --- Find dependencies ---
find_package(Qt6 COMPONENTS Widgets Gui Core Network Test REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SPICE_CLIENT IMPORTED_TARGET spice-client-glib-2.0)
    pkg_check_modules(GLIB IMPORTED_TARGET glib-2.0)
endif()
if(SPICE_CLIENT_FOUND)
    message(STATUS "SPICE client: found (${SPICE_CLIENT_VERSION})")
else()
    message(STATUS "SPICE client: not found (VNC-only mode)")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- gRPC + Protobuf ---
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    # Fallback: find via pkg-config
    if(PkgConfig_FOUND)
        pkg_check_modules(GRPC IMPORTED_TARGET grpc++ grpc)
    endif()
endif()

# --- Emulator gRPC client ---
add_library(rex_emu STATIC
    src/emu/emulator_process.cpp
    src/emu/grpc_display.cpp
    src/emu/window_grabber.cpp
    src/emu/generated/emulator_controller.pb.cc
    src/emu/generated/emulator_controller.grpc.pb.cc
)
target_include_directories(rex_emu PUBLIC src/emu src/emu/generated)
if(gRPC_FOUND)
    target_link_libraries(rex_emu PUBLIC
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
        gRPC::grpc++ protobuf::libprotobuf
    )
elseif(GRPC_FOUND)
    target_link_libraries(rex_emu PUBLIC
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
        PkgConfig::GRPC protobuf::libprotobuf
    )
else()
    message(WARNING "gRPC not found — emulator gRPC client will not link")
    target_link_libraries(rex_emu PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
endif()
if(APPLE)
    target_link_libraries(rex_emu PUBLIC "-framework CoreGraphics")
endif()

# --- QEMU process management ---
add_library(rex_qemu STATIC
    src/qemu/qmp_client.cpp
    src/qemu/qemu_config.cpp
    src/qemu/qemu_process.cpp
)
target_include_directories(rex_qemu PUBLIC src/qemu)
target_link_libraries(rex_qemu PUBLIC Qt6::Core Qt6::Network)

# --- VNC client ---
add_library(rex_vnc STATIC
    src/vnc/vnc_client.cpp
)
target_include_directories(rex_vnc PUBLIC src/vnc)
target_link_libraries(rex_vnc PUBLIC Qt6::Core Qt6::Gui Qt6::Network)

# --- SPICE client (optional — requires libspice-client-glib) ---
if(SPICE_CLIENT_FOUND AND GLIB_FOUND)
    add_library(rex_spice STATIC
        src/spice/spice_client.cpp
        src/spice/spice_display.cpp
        src/spice/spice_input.cpp
    )
    target_include_directories(rex_spice PUBLIC src/spice)
    target_link_libraries(rex_spice PUBLIC Qt6::Core Qt6::Gui PkgConfig::SPICE_CLIENT PkgConfig::GLIB)
    set(REX_HAS_SPICE ON)
else()
    set(REX_HAS_SPICE OFF)
endif()

# --- GUI executable ---
add_executable(rexplayer
    src/gui/main.cpp
    src/gui/mainwindow.cpp
    src/gui/display_widget.cpp
    src/gui/input_handler.cpp
    src/gui/settings_dialog.cpp
    src/gui/keymap_editor.cpp
    src/gui/frida_panel.cpp
)
target_link_libraries(rexplayer PRIVATE
    rex_emu rex_qemu rex_vnc
    Qt6::Widgets Qt6::Gui Qt6::Core
)
if(REX_HAS_SPICE)
    target_link_libraries(rexplayer PRIVATE rex_spice)
    target_compile_definitions(rexplayer PRIVATE REX_HAS_SPICE=1)
endif()
target_include_directories(rexplayer PRIVATE src/gui)

# --- Tests ---
if(REX_ENABLE_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()
