# =============================================================================
# lang_gateway — 推理语言网关产品库（products 产品仓，M5-2 迁出）
#
# 双模式构建契约（0.1.19 §1.3 机制核外迁；台账 §256）：
#   embedded   —— agentrt 根 add_subdirectory 纳入本目录时
#                 CMAKE_SOURCE_DIR(=agentrt) ≠ CMAKE_CURRENT_SOURCE_DIR(=本目录)，
#                 下方 preamble 整块跳过；依赖检测、平台宏、合规注入、
#                 commons 目标均由根统一提供（embedded 形态字节不变、零回归）。
#   standalone —— cmake -S products/lang_gateway -B <出树目录> 产品仓独立
#                 configure/构建/测试；经 AGENTRT_ROOT 复用 agentrt 管理仓的
#                 cmake 模块与 commons 原子件。
#
# 机制/策略切分：数据类型契约与 ops 注入表在机制核
# （atoms/coreloopthree/include/airy_lang_gw_ops.h）；本库为策略载荷，
# 编译出的 airy_lang_gateway 静态库由 daemons 链接并经
# are_ops_set_lang_gw() 注入，机制核不直接依赖本库。
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
    project(AgentOS-LangGateway VERSION ${AIRY_VERSION_NUM} LANGUAGES C)

    # 源码区内严禁构建：in-source 与构建树嵌于源码树内均 FATAL
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
        message(FATAL_ERROR
            "lang_gateway in-source builds are not allowed! Use: cmake -S . -B <external-build-dir>")
    endif()
    cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${CMAKE_BINARY_DIR}" NORMALIZE _LANG_GW_BIN_IN_SRC)
    if(_LANG_GW_BIN_IN_SRC)
        message(FATAL_ERROR
            "lang_gateway build directory MUST be outside the source tree: ${CMAKE_BINARY_DIR}")
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
    set(LANG_GW_COMMONS_DIR "${AGENTRT_ROOT}/commons")
    set(LANG_GW_CONTRACT_DIR "${AGENTRT_ROOT}/atoms/coreloopthree/include")
    set(AIRY_COREKERN_INCLUDE_DIR "${AGENTRT_ROOT}/atoms/corekern/include")
endif()

set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

airy_print_info("[LANG_GATEWAY] Configuring inference language gateway product library")

option(BUILD_TESTS "Build tests" ON)

# embedded 根树锚点兜底（standalone 已由 preamble 置好）
if(NOT LANG_GW_COMMONS_DIR)
    set(LANG_GW_COMMONS_DIR ${CMAKE_SOURCE_DIR}/commons)
endif()
if(NOT LANG_GW_CONTRACT_DIR)
    set(LANG_GW_CONTRACT_DIR "${CMAKE_SOURCE_DIR}/atoms/coreloopthree/include")
endif()
if(NOT AIRY_COREKERN_INCLUDE_DIR)
    set(AIRY_COREKERN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/atoms/corekern/include")
endif()

# cJSON 检测：embedded 复用根检测 CACHE（coreloopthree 提升的 AIRY_CJSON_*）；
# standalone 自行 pkg 探测。仅 calibrator.c 消费，且带无 cJSON 手写扫描真实
# 回退——未检出时编译降级路径，非桩实现。
if(PkgConfig_FOUND AND NOT AIRY_CJSON_FOUND)
    pkg_check_modules(CJSON QUIET libcjson)
endif()
set(_lang_gw_cjson_inc "")
set(_lang_gw_cjson_libs "")
if(AIRY_CJSON_FOUND AND AIRY_CJSON_LIBRARIES)
    set(_lang_gw_cjson_inc "${AIRY_CJSON_INCLUDE_DIRS}")
    set(_lang_gw_cjson_libs "${AIRY_CJSON_LIBRARIES}")
elseif(CJSON_FOUND OR CJSON_LIBRARIES)
    set(_lang_gw_cjson_inc "${CJSON_INCLUDE_DIRS}")
    set(_lang_gw_cjson_libs "${CJSON_LIBRARIES}")
endif()

set(LANG_GATEWAY_SOURCES
    src/lang_gateway.c
    src/signal_extractor.c
    src/lang_router.c
    src/system_prompt.c
    src/output_post_processor.c
    src/calibrator.c
)
set(LANG_GATEWAY_HEADERS
    include/lang_gateway.h
    src/canonical.h
)

# IPC ops 存储符号：embedded 链接核内 airy_ipc_ops 小库；standalone 无该
# 目标，直编其唯一成员（ipc_ops.c 为零依赖抽象表存储，非策略载荷）。
set(_lang_gw_ipc_ops_src "")
if(NOT TARGET airy_ipc_ops)
    set(_lang_gw_ipc_ops_src "${LANG_GW_CONTRACT_DIR}/../src/ops/ipc_ops.c")
    list(APPEND LANG_GATEWAY_SOURCES ${_lang_gw_ipc_ops_src})
endif()

add_library(airy_lang_gateway STATIC ${LANG_GATEWAY_SOURCES} ${LANG_GATEWAY_HEADERS})

target_include_directories(airy_lang_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 契约面 PUBLIC 传播：本库公共头含 include 的契约头裸名（airy_lang_gw_ops.h
# → airy_ipc_ops.h 拉 commons IPC/SD/svc 契约头），消费者须自包含解析。
target_include_directories(airy_lang_gateway PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${LANG_GW_CONTRACT_DIR}>
    $<BUILD_INTERFACE:${AIRY_COREKERN_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/platform/include>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/include>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/error>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/memory>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/logging>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/ipc>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/sd>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/svc>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/types>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/utils/sync>
    $<BUILD_INTERFACE:${LANG_GW_COMMONS_DIR}/include>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/agentrt/lang_gateway>
)

if(_lang_gw_ipc_ops_src)
    target_include_directories(airy_lang_gateway PRIVATE ${LANG_GW_CONTRACT_DIR})
endif()

target_link_libraries(airy_lang_gateway PUBLIC airy_common)
if(TARGET airy_ipc_ops)
    target_link_libraries(airy_lang_gateway PUBLIC airy_ipc_ops)
endif()

if(_lang_gw_cjson_libs)
    target_compile_definitions(airy_lang_gateway PRIVATE AIRY_HAS_CJSON=1)
    target_include_directories(airy_lang_gateway PRIVATE ${_lang_gw_cjson_inc})
    target_link_libraries(airy_lang_gateway PRIVATE ${_lang_gw_cjson_libs})
endif()

if(MSVC)
    target_compile_options(airy_lang_gateway PRIVATE /W4)
    target_link_libraries(airy_lang_gateway PRIVATE ws2_32 advapi32)
else()
    target_compile_options(airy_lang_gateway PRIVATE
        -Wall -Wextra -Wpedantic
        -Wformat=2 -Wformat-security
        -Werror=implicit-function-declaration -Werror=return-type
        -fstack-protector-strong
        -fstack-clash-protection
        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
        -fvisibility=hidden
        -fno-omit-frame-pointer
    )
    target_link_libraries(airy_lang_gateway PRIVATE m)
    if(UNIX AND NOT APPLE)
        target_link_options(airy_lang_gateway PRIVATE
            -Wl,-z,relro
            -Wl,-z,now
            -Wl,-z,noexecstack
        )
    elseif(APPLE)
        target_link_options(airy_lang_gateway PRIVATE -Wl,-dead_strip)
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()

    add_executable(test_lang_gateway tests/unit/test_lang_gateway.c)
    target_compile_definitions(test_lang_gateway PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_lang_gateway PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LANG_GW_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
        ${_lang_gw_cjson_inc}
    )
    target_link_libraries(test_lang_gateway PRIVATE airy_lang_gateway)
    if(NOT MSVC)
        target_link_libraries(test_lang_gateway PRIVATE pthread dl)
    endif()
    add_test(NAME lang_gateway_tests COMMAND test_lang_gateway)
endif()

install(TARGETS airy_lang_gateway
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib/static
)

install(FILES include/lang_gateway.h
    DESTINATION include/agentrt/lang_gateway
)

install(FILES src/canonical.h
    DESTINATION include/agentrt/lang_gateway
)
