# =============================================================================
# canary — 金丝雀自演化机制核（0.1.20 §7.0.2，products 产品仓）
#
# 双模式构建契约（镜像 cognition 范式）：
#   embedded   —— agentrt 根 add_subdirectory 纳入本目录时 preamble
#                 整块跳过；版本 SSoT、平台宏由根统一提供。
#   standalone —— cmake -S products/canary -B <出树目录> 独立构建测试；
#                 经 AGENTRT_ROOT 承接版本 SSoT（仅 VERSION，零代码依赖）。
#
# 机制核定位：零第三方依赖（C11 + libc + <stdatomic.h>），五态机 /
# 爆炸半径守卫 / kill switch 四通道 / overlay manifest（base SHA-256）。
# 策略面在宿主（dpolicy 等）经挂载点接线，本库不携带默认策略。
# =============================================================================
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)

    if(NOT AGENTRT_ROOT)
        if(DEFINED ENV{AGENTRT_ROOT})
            set(AGENTRT_ROOT "$ENV{AGENTRT_ROOT}")
        else()
            set(AGENTRT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../agentrt")
        endif()
    endif()
    get_filename_component(AGENTRT_ROOT "${AGENTRT_ROOT}" ABSOLUTE)
    if(NOT EXISTS "${AGENTRT_ROOT}/VERSION")
        message(FATAL_ERROR
            "standalone build requires agentrt VERSION SSoT: AGENTRT_ROOT=${AGENTRT_ROOT} invalid (pass -DAGENTRT_ROOT=<agentrt> or set env AGENTRT_ROOT)")
    endif()
    file(STRINGS "${AGENTRT_ROOT}/VERSION" AIRY_VERSION_RAW LIMIT_COUNT 1)
    string(STRIP "${AIRY_VERSION_RAW}" AIRY_VERSION_RAW)
    if(NOT AIRY_VERSION_RAW MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
        message(FATAL_ERROR "版本 SSoT 非法：VERSION 内容 '${AIRY_VERSION_RAW}' 不符合 X.Y.Z")
    endif()
    string(REGEX REPLACE "([0-9.]+).*" "\\1" AIRY_VERSION_NUM "${AIRY_VERSION_RAW}")
    project(AgentOS-Canary VERSION ${AIRY_VERSION_NUM} LANGUAGES C)

    # 源码区内严禁构建：in-source 与构建树嵌于源码树内均 FATAL
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
        message(FATAL_ERROR
            "canary in-source builds are not allowed! Use: cmake -S . -B <external-build-dir>")
    endif()
    cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${CMAKE_BINARY_DIR}" NORMALIZE _CAN_BIN_IN_SRC)
    if(_CAN_BIN_IN_SRC)
        message(FATAL_ERROR
            "canary build directory MUST be outside the source tree: ${CMAKE_BINARY_DIR}")
    endif()

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    enable_testing()
endif()

set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

option(BUILD_TESTS "Build tests" ON)

# SE-01 合规嵌入态实现层豁免（先例：atoms/corekern/freestanding 逐源
# 注入）：根构建以 -include banned_functions.h 全域毒化 libc 分配原语，
# canary_core.c 的 canary_new/canary_free 是模块唯一分配边界（不依赖
# commons 分配器以维持零 agentrt 依赖），按实现层通道豁免。standalone
# 构建无 banned 头注入，此定义无副作用。
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set_source_files_properties(src/canary_core.c PROPERTIES
        COMPILE_DEFINITIONS "AIRY_COMPLIANCE_IMPL")
endif()

add_library(airy_canary STATIC
    src/canary_core.c
    src/canary_manifest.c
    src/canary_sha256.c
    src/canary_internal.h
    include/canary_sha256.h)

target_include_directories(airy_canary
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    # <stdatomic.h> 在 MSVC 14.3x+ 需显式开启实验性 C11 原子支持，
    # 否则 vcruntime_c11_stdatomic.h 直接 C1189（release.yml windows
    # 腿实证）；/std:c11 由顶层 CMAKE_C_STANDARD=11 统一下发。
    target_compile_options(airy_canary PRIVATE /W4 /experimental:c11atomics)
else()
    target_compile_options(airy_canary PRIVATE
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration
        -fvisibility=hidden)
endif()

if(BUILD_TESTS)
    add_subdirectory(tests/unit)
endif()
