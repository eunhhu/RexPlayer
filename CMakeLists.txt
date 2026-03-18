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
find_package(Qt6 COMPONENTS Widgets Gui Core Network REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPICE_CLIENT REQUIRED spice-client-glib-2.0)
pkg_check_modules(GLIB REQUIRED glib-2.0)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- Libraries will be added by subsequent tasks ---

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
