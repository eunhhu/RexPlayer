# Tests
add_executable(rex_hal_tests
    hal_tests/test_types.cpp
    hal_tests/test_device_manager.cpp
    hal_tests/test_boot_arm64.cpp
    hal_tests/test_kvm_integration.cpp
    hal_tests/test_memory_manager.cpp
)
target_link_libraries(rex_hal_tests PRIVATE
    rex_hal
    rex_vmm
    rex_devices_legacy
    GTest::gtest_main
)
target_include_directories(rex_hal_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devices
    ${CMAKE_SOURCE_DIR}/src/hal
)

include(GoogleTest)
gtest_discover_tests(rex_hal_tests)

# GPU tests
add_executable(rex_gpu_tests
    gpu_tests/test_software_renderer.cpp
)
target_link_libraries(rex_gpu_tests PRIVATE
    rex_gpu
    GTest::gtest_main
)
gtest_discover_tests(rex_gpu_tests)
