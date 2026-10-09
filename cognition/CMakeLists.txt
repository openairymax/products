# =============================================================================
# cognition — 认知分发策略产品库（products 产品仓，M5-4 迁出）
#
# 双模式构建契约（0.1.19 §1.3 机制核外迁；台账 §261）：
#   embedded   —— agentrt 根 add_subdirectory 纳入本目录时
#                 CMAKE_SOURCE_DIR(=agentrt) ≠ CMAKE_CURRENT_SOURCE_DIR(=本目录)，
#                 下方 preamble 整块跳过；依赖检测、平台宏、合规注入、
#                 commons 目标均由根统一提供（embedded 形态字节不变、零回归）。
#   standalone —— cmake -S products/cognition -B <出树目录> 产品仓独立
#                 configure/构建/测试；经 AGENTRT_ROOT 复用 agentrt 管理仓的
#                 cmake 模块与 commons 原子件。
#
# 机制/策略切分：分发策略对象契约 airy_dispatching_strategy_t 在机制核
# （atoms/coreloopthree/include/cognition.h），本库为策略载荷——加权 / 轮询 /
# 优先级 / ML 四类分发策略实现。机制核不链接本库、不携带默认策略：消费者
# 以本库工厂创建策略实例，经 airy_cognition_set_dispatching_strategy() 或
# airy_cognition_create*_take() 的 disp_strategy 形参注入（TRANSFER 所有权）。
# =============================================================================
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)

    # AGENTRT_ROOT 三态：-D 缓存变量 → 环境变量 → 默认相对路径
    if(NOT AGENTRT_ROOT)
        if(DEFINED ENV{AGENTRT_ROOT})
            set(AGENTRT_ROOT "$ENV{AGENTRT_ROOT}")
        else()
            set(AGENTRT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../agentrt")
        endif()
    endif()
    get_filename_component(AGENTRT_ROOT "${AGENTRT_ROOT}" ABSOLUTE)
    if(NOT EXISTS "${AGENTRT_ROOT}/commons/CMakeLists.txt")
        message(FATAL_ERROR
            "standalone build requires agentrt commons: AGENTRT_ROOT=${AGENTRT_ROOT} invalid (pass -DAGENTRT_ROOT=<agentrt> or set env AGENTRT_ROOT)")
    endif()

    # 版本 SSoT：承接 agentrt/VERSION（缺失/非法大声失败，镜像根 DT-13 形态）
    file(STRINGS "${AGENTRT_ROOT}/VERSION" AIRY_VERSION_RAW LIMIT_COUNT 1)
    string(STRIP "${AIRY_VERSION_RAW}" AIRY_VERSION_RAW)
    if(NOT AIRY_VERSION_RAW MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
        message(FATAL_ERROR "版本 SSoT 非法：VERSION 内容 '${AIRY_VERSION_RAW}' 不符合 X.Y.Z")
    endif()
    string(REGEX REPLACE "([0-9.]+).*" "\\1" AIRY_VERSION_NUM "${AIRY_VERSION_RAW}")
    project(AgentOS-Cognition VERSION ${AIRY_VERSION_NUM} LANGUAGES C)

    # 源码区内严禁构建：in-source 与构建树嵌于源码树内均 FATAL
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
        message(FATAL_ERROR
            "cognition in-source builds are not allowed! Use: cmake -S . -B <external-build-dir>")
    endif()
    cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${CMAKE_BINARY_DIR}" NORMALIZE _COG_BIN_IN_SRC)
    if(_COG_BIN_IN_SRC)
        message(FATAL_ERROR
            "cognition build directory MUST be outside the source tree: ${CMAKE_BINARY_DIR}")
    endif()

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # 复用 agentrt cmake 模块：平台检测 + 依赖检测收口 + 宏双路下发
    list(APPEND CMAKE_MODULE_PATH "${AGENTRT_ROOT}/cmake")
    include(airy_print)
    include(platform)
    include(dependencies)

    find_package(PkgConfig QUIET)

    airy_find_required_deps()
    airy_find_optional_deps()
    airy_detect_platform()
    airy_apply_dep_defs()

    # 版本宏 SSoT 注入（commons airyrt_version.h 消费面）
    add_compile_definitions(AIRYRT_VERSION="${AIRY_VERSION_RAW}")

    # 合规严格模式：镜像根注入面（非 MSVC -include 禁令头；MSVC /FI 强制
    # 预包含）。embedded 下本块整体跳过，由根注入，无双重下发（IRON-8）
    option(AIRY_COMPLIANCE_STRICT "Enable strict compliance mode (poison unsafe functions)" ON)
    if(NOT MSVC)
        if(AIRY_COMPLIANCE_STRICT)
            add_compile_definitions(AIRY_COMPLIANCE_STRICT)
            add_compile_options("-include" "${AGENTRT_ROOT}/commons/utils/compliance/banned_functions.h")
        endif()
    else()
        add_compile_options(/FI"${AGENTRT_ROOT}/cmake/windows_preinclude.h")
    endif()

    enable_testing()

    # commons 原子件装配（standalone 下 airy_common 唯一创建点）；以
    # normal 变量阻断其 tests 挂载（CMP0077 NEW：option() 让位先存变量）
    set(BUILD_TESTS OFF)
    add_subdirectory("${AGENTRT_ROOT}/commons" commons)
    unset(BUILD_TESTS)

    # body 断链回退锚：commons / 契约头 / corekern 头均指向 agentrt 管理仓
    set(COG_COMMONS_DIR "${AGENTRT_ROOT}/commons")
    set(COG_CONTRACT_DIR "${AGENTRT_ROOT}/atoms/coreloopthree/include")
    set(AIRY_COREKERN_INCLUDE_DIR "${AGENTRT_ROOT}/atoms/corekern/include")
endif()

set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

airy_print_info("[COGNITION] Configuring cognition dispatch strategy product library")

option(BUILD_TESTS "Build tests" ON)

# embedded 根树锚点兜底（standalone 已由 preamble 置好）
if(NOT COG_COMMONS_DIR)
    set(COG_COMMONS_DIR ${CMAKE_SOURCE_DIR}/commons)
endif()
if(NOT COG_CONTRACT_DIR)
    set(COG_CONTRACT_DIR "${CMAKE_SOURCE_DIR}/atoms/coreloopthree/include")
endif()
if(NOT AIRY_COREKERN_INCLUDE_DIR)
    set(AIRY_COREKERN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/atoms/corekern/include")
endif()

set(COGNITION_SOURCES
    src/weighted.c
    src/priority.c
    src/round_robin.c
    src/ml_based.c
    src/hierarchical.c
    src/ml_planner.c
)
set(COGNITION_HEADERS
    include/dispatch_strategy.h
    include/plan_strategy.h
)

add_library(airy_cognition_strategy STATIC ${COGNITION_SOURCES} ${COGNITION_HEADERS})

target_include_directories(airy_cognition_strategy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 契约面 PUBLIC 传播：本库公共头含 include 的契约头裸名（dispatch_strategy.h
# → agent_registry.h / cognition.h），consumer 及 standalone 均须自包含解析。
target_include_directories(airy_cognition_strategy PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${COG_CONTRACT_DIR}>
    $<BUILD_INTERFACE:${AIRY_COREKERN_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/platform/include>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/utils/include>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/utils/error>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/utils/memory>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/utils/logging>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/utils/string>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/utils/cognition>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/utils/types>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/utils/sync>
    $<BUILD_INTERFACE:${COG_COMMONS_DIR}/include>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/agentrt/cognition>
)

target_link_libraries(airy_cognition_strategy PUBLIC airy_common)

if(MSVC)
    target_compile_options(airy_cognition_strategy PRIVATE /W4)
    target_link_libraries(airy_cognition_strategy PRIVATE ws2_32 advapi32)
else()
    target_compile_options(airy_cognition_strategy PRIVATE
        -Wall -Wextra -Wpedantic
        -Wformat=2 -Wformat-security
        -Werror=implicit-function-declaration -Werror=return-type
        -fstack-protector-strong
        -fstack-clash-protection
        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
        -fvisibility=hidden
        -fno-omit-frame-pointer
    )
    target_link_libraries(airy_cognition_strategy PRIVATE m)
    if(UNIX AND NOT APPLE)
        target_link_options(airy_cognition_strategy PRIVATE
            -Wl,-z,relro
            -Wl,-z,now
            -Wl,-z,noexecstack
        )
    elseif(APPLE)
        target_link_options(airy_cognition_strategy PRIVATE -Wl,-dead_strip)
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()

    add_executable(test_dispatch_strategy tests/unit/test_dispatch_strategy.c)
    target_compile_definitions(test_dispatch_strategy PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_dispatch_strategy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
    )
    target_link_libraries(test_dispatch_strategy PRIVATE airy_cognition_strategy)
    if(NOT MSVC)
        target_link_libraries(test_dispatch_strategy PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_strategy_tests COMMAND test_dispatch_strategy)

    add_executable(test_plan_strategy tests/unit/test_plan_strategy.c)
    target_compile_definitions(test_plan_strategy PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_plan_strategy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
    )
    target_link_libraries(test_plan_strategy PRIVATE airy_cognition_strategy)
    if(NOT MSVC)
        target_link_libraries(test_plan_strategy PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_plan_tests COMMAND test_plan_strategy)
endif()

install(TARGETS airy_cognition_strategy
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib/static
)

install(FILES include/dispatch_strategy.h include/plan_strategy.h
    DESTINATION include/agentrt/cognition
)
