include(GoogleTest)

add_executable(test_qmp_client qemu_tests/test_qmp_client.cpp)
target_link_libraries(test_qmp_client PRIVATE
    rex_qemu GTest::gtest_main Qt6::Core Qt6::Network Qt6::Test
)
gtest_discover_tests(test_qmp_client)
