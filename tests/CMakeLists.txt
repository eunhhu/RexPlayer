# Tests
add_executable(rex_hal_tests
    hal_tests/test_types.cpp
    hal_tests/test_device_manager.cpp
    hal_tests/test_kvm_integration.cpp
)
target_link_libraries(rex_hal_tests PRIVATE
    rex_hal
    rex_vmm
    rex_devices_legacy
    GTest::gtest_main
)
target_include_directories(rex_hal_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devices
)

include(GoogleTest)
gtest_discover_tests(rex_hal_tests)
