# 单元测试：金丝雀状态机（真实 IO，禁桩）
add_executable(test_canary test_canary.c)
target_link_libraries(test_canary PRIVATE airy_canary)

if(MSVC)
    target_compile_options(test_canary PRIVATE /W4)
else()
    target_compile_options(test_canary PRIVATE
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
endif()

add_test(NAME canary_unit COMMAND test_canary)
