# =============================================================================
# cognition — 认知策略产品库（分发 / 规划 / 协调，products 产品仓，M5-4 迁出）
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

# cJSON 检测：embedded 复用根检测 CACHE（coreloopthree 提升的 AIRY_CJSON_*）；
# standalone 经 pkg-config 或已导入的 CJSON_* 变量探测。镜像 lang_gateway 范式。
if(PkgConfig_FOUND AND NOT AIRY_CJSON_FOUND)
    pkg_check_modules(CJSON QUIET libcjson)
endif()
set(_cog_cjson_inc "")
set(_cog_cjson_libs "")
if(AIRY_CJSON_FOUND AND AIRY_CJSON_LIBRARIES)
    set(_cog_cjson_inc "${AIRY_CJSON_INCLUDE_DIRS}")
    set(_cog_cjson_libs "${AIRY_CJSON_LIBRARIES}")
elseif(CJSON_FOUND OR CJSON_LIBRARIES)
    set(_cog_cjson_inc "${CJSON_INCLUDE_DIRS}")
    set(_cog_cjson_libs "${CJSON_LIBRARIES}")
endif()

set(COGNITION_SOURCES
    # 规划策略载荷族（0.1.19 M5-4 §262/§271，自 atoms/coreloopthree
    # src/cognition/think/planner 迁入并归格 src/planner/；机制核仅留
    # llm_client 机制件 + airy_plan_ops.h 契约 + are_ops_set/get_plan()
    # 分发面，载荷经 payload_registry 注表装配）
    src/planner/hierarchical.c
    src/planner/ml_based.c
    src/planner/ml_planner.c
    src/planner/reactive.c
    src/planner/reflective.c
    src/planner/reflective_llm_plan.c
    src/weighted.c
    src/priority.c
    src/round_robin.c
    src/coord_dual.c
    src/coord_majority.c
    src/coord_weighted.c
    src/coord_arbiter.c
    src/coord_adapter.c
    src/cog_review.c
    src/gccp.c
    src/gccp_llm_json.c
    src/gccp_heuristic.c
    # GRAD 计划级批判环策略载荷（0.1.19 M5-4 §267，自 atoms/coreloopthree
    # src/cognition/grad 迁入；机制核仅留 grad.h 契约 + 机制面分发）
    src/grad_verifier.c
    src/grad_coordinator.c
    src/grad_llm_ws.c
    src/grad_llm_call.c
    src/grad_llm_s2_plan.c
    src/grad_llm_s1_arbiter.c
    src/grad_run.c
    # MC/TC 认知基座策略载荷（0.1.19 M5-4 §268，自 atoms/coreloopthree
    # src/cognition/think/foundation 迁入；机制核仅留 tc.h/mc.h 契约 +
    # ops 注入表，daemon 侧经 payload_registry 注表）
    src/foundation/metacognition.c
    src/foundation/thinking_chain.c
    src/foundation/mc/mc_calibrate.c
    src/foundation/mc/mc_correct.c
    src/foundation/mc/mc_evaluate.c
    src/foundation/mc/mc_learn.c
    src/foundation/tc/tc_attention.c
    src/foundation/tc/tc_context_window.c
    src/foundation/tc/tc_memory.c
    src/foundation/tc/tc_monitor.c
    src/foundation/tc/tc_recovery.c
    src/foundation/tc/tc_step.c
    src/foundation/tc/tc_working_memory.c
    # 意图解析策略载荷（0.1.19 M5-4 §269，自 atoms/coreloopthree
    # src/cognition/think/intent 迁入；机制核仅留 intent.h 契约 +
    # are_ops_set/get_intent() 分发面，sc Phase0 缺席 ENOSYS 中断）
    src/intent/intent_parser.c
    src/intent/intent_rule_engine.c
    src/intent/intent_entity_extractor.c
    src/intent/intent_parser_ops.c
    src/intent/intent_classifier.c
    src/intent/entity_extractor.c
    # 认知进化策略载荷（0.1.19 M5-4 §270，自 atoms/coreloopthree
    # src/cognition/think/ext 迁入；零机制侧消费者的独立认知策略域。
    # MAC 协作运行时三源系 sched_d 硬依赖的机制，留机制核不迁）
    src/ext/cognitive_evolution.c
    src/payload_registry.c
)
set(COGNITION_HEADERS
    include/dispatch_strategy.h
    include/plan_strategy.h
    include/coord_strategy.h
    include/cog_review_strategy.h
    include/gccp_strategy.h
    include/grad_strategy.h
    include/payload_registry.h
    include/cognitive_evolution.h
)

# 机制 ops 小库（airy_llm_ops）：协调策略载荷经注入表调用 LLM。embedded 形态
# 由 atoms 提供 airy_llm_ops 目标；standalone 形态（未纳入 atoms）直编其唯一
# 实现单元 src/ops/llm_ops.c（零依赖），使本库自包含。镜像 lang_gateway 范式。
set(COG_LLM_OPS_SRC "")
if(NOT TARGET airy_llm_ops)
    set(COG_LLM_OPS_SRC "${COG_CONTRACT_DIR}/../src/ops/llm_ops.c")
endif()

# 规划策略载荷支撑单元（0.1.19 M5-4 §271）：reactive/reflective 依赖
# 机制核 llm_client（LLM 服务生命周期）与 llm_svc_adapter（IPC 通道，
# adapter 三源互调 + are_ops_get_ipc）+ ops_injection（契约存储面）。
# embedded 形态由 airy_cognition 目标提供；standalone 形态（未纳入
# atoms）直编对应实现单元，使本库自包含。镜像 COG_LLM_OPS_SRC 范式。
set(COG_PLAN_BASE_SRC "")
if(NOT TARGET airy_cognition)
    set(COG_PLAN_BASE_SRC
        "${COG_CONTRACT_DIR}/../src/cognition/think/planner/llm_client.c"
        "${COG_CONTRACT_DIR}/../src/cognition/think/adapter/llm_svc_adapter.c"
        "${COG_CONTRACT_DIR}/../src/cognition/think/adapter/llm_svc_adapter_json.c"
        "${COG_CONTRACT_DIR}/../src/cognition/think/adapter/llm_svc_adapter_stream.c"
        # llm_svc_adapter 经 are_ops_get_ipc 消费 IPC 注表面；该机制存储件
        # （零依赖）在 embedded 由 airy_coreloopthree 闭包承接，standalone
        # 直编本体（镜像 ops_injection.c 范式，非桩）。
        "${COG_CONTRACT_DIR}/../src/ops/ipc_ops.c"
        "${COG_CONTRACT_DIR}/../src/ops/ops_injection.c")
endif()

add_library(airy_cognition_strategy STATIC
    ${COGNITION_SOURCES} ${COGNITION_HEADERS} ${COG_LLM_OPS_SRC} ${COG_PLAN_BASE_SRC})

target_include_directories(airy_cognition_strategy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/foundation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/intent
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ext
    ${CMAKE_CURRENT_SOURCE_DIR}/src/planner
    # §271 迁入件消费机制核内部头（llm_client.h/llm_svc_adapter.h）；
    # airy_cognition 的 think src include 为 PRIVATE 不传播，直编链须
    # 显式解析；llm_svc_adapter.c 经 airy_ipc_ops.h 传递依赖
    # daemon_rpc_client.h（commons/utils/ipc/）。
    "${COG_CONTRACT_DIR}/../src/cognition/think"
    "${COG_CONTRACT_DIR}/../src/cognition/think/planner"
    "${COG_CONTRACT_DIR}/../src/cognition/think/adapter"
    ${COG_COMMONS_DIR}/utils/ipc
    # llm_svc_adapter.c 消费 airy_quality.h（E-1/E-3/E-6 质量宏）；
    # embedded 由 atoms 库闭包解析，standalone 直编链须显式给出。
    ${COG_COMMONS_DIR}/utils/quality
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

# embedded：链接 airy_core 以继承其 PUBLIC 的 AIRY_USE_SCHEDULER_THREAD_IMPL
# 定义，使 CPR 并行 worker 走 corekern 调度线程 SSoT（G1 守恒）；standalone
# 无 airy_core，cog_review 的线程调用回退平台别名（可接受）。
if(TARGET airy_core)
    target_link_libraries(airy_cognition_strategy PUBLIC airy_core)
endif()

if(TARGET airy_llm_ops)
    target_link_libraries(airy_cognition_strategy PUBLIC airy_llm_ops)
endif()

# 意图解析载荷（intent_parser.c，0.1.19 M5-4 §269 迁入）保留机制侧支撑
# 符号依赖（airy_generate_uuid/airy_gccp_goal_free）；embedded 经 PRIVATE
# 链承接（静态库 LINK_ONLY 传播至最终链接），standalone 无机制核，测试
# 目标直编对应实现单元（镜像 COG_GCCP_TEST_SRCS 范式）。
if(TARGET airy_coreloopthree)
    target_link_libraries(airy_cognition_strategy PRIVATE airy_coreloopthree)
endif()

# cJSON：cog_review 汇总决策 JSON（embedded/standalone 一致）
if(_cog_cjson_libs)
    target_compile_definitions(airy_cognition_strategy PRIVATE AIRY_HAS_CJSON=1)
    target_include_directories(airy_cognition_strategy PRIVATE ${_cog_cjson_inc})
    target_link_libraries(airy_cognition_strategy PRIVATE ${_cog_cjson_libs})
endif()

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

    add_executable(test_coord_strategy tests/unit/test_coord_strategy.c)
    target_compile_definitions(test_coord_strategy PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_coord_strategy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
    )
    target_link_libraries(test_coord_strategy PRIVATE airy_cognition_strategy)
    if(NOT MSVC)
        target_link_libraries(test_coord_strategy PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_coord_tests COMMAND test_coord_strategy)

    add_executable(test_cog_review_strategy tests/unit/test_cog_review_strategy.c)
    target_compile_definitions(test_cog_review_strategy PRIVATE AIRY_COMPLIANCE_IMPL)
    if(_cog_cjson_libs)
        target_compile_definitions(test_cog_review_strategy PRIVATE AIRY_HAS_CJSON=1)
    endif()
    target_include_directories(test_cog_review_strategy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
        ${_cog_cjson_inc}
    )
    target_link_libraries(test_cog_review_strategy PRIVATE airy_cognition_strategy)
    if(_cog_cjson_libs)
        target_link_libraries(test_cog_review_strategy PRIVATE ${_cog_cjson_libs})
    endif()
    if(NOT MSVC)
        target_link_libraries(test_cog_review_strategy PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_cog_review_tests COMMAND test_cog_review_strategy)

    # GCCP 策略载荷测试：数据模型释放（gccp_goal.c）与 ops 注入表
    # （ops_injection.c）符号在 embedded 由 airy_coreloopthree 目标提供；
    # standalone 直编其实现单元（纯数据操作/纯存储，零额外依赖，镜像
    # COG_LLM_OPS_SRC 范式）。
    set(COG_GCCP_TEST_SRCS "")
    if(NOT TARGET airy_coreloopthree)
        set(COG_GCCP_TEST_SRCS
            "${COG_CONTRACT_DIR}/../src/cognition/gccp/gccp_goal.c"
            "${COG_CONTRACT_DIR}/../src/ops/ops_injection.c")
    endif()
    add_executable(test_gccp_strategy tests/unit/test_gccp_strategy.c ${COG_GCCP_TEST_SRCS})
    target_compile_definitions(test_gccp_strategy PRIVATE AIRY_COMPLIANCE_IMPL)
    if(_cog_cjson_libs)
        target_compile_definitions(test_gccp_strategy PRIVATE AIRY_HAS_CJSON=1)
    endif()
    target_include_directories(test_gccp_strategy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
        ${_cog_cjson_inc}
    )
    target_link_libraries(test_gccp_strategy PRIVATE airy_cognition_strategy)
    if(TARGET airy_coreloopthree)
        target_link_libraries(test_gccp_strategy PRIVATE airy_coreloopthree)
    endif()
    if(_cog_cjson_libs)
        target_link_libraries(test_gccp_strategy PRIVATE ${_cog_cjson_libs})
    endif()
    if(NOT MSVC)
        target_link_libraries(test_gccp_strategy PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_gccp_tests COMMAND test_gccp_strategy)

    # GRAD 计划级批判环策略载荷测试：协调器/验证器/工作区/S2 计划/B 仲裁/
    # 运行闭环等被测符号全部位于本库（含 grad_run）；计划数据模型
    # （airy_task_plan_free/airy_plan_parse，plan_model.c）在 embedded 由
    # airy_cognition 目标提供，standalone 直编其实现单元（零机制依赖，
    # 镜像 COG_GCCP_TEST_SRCS 范式）；cJSON 依赖镜像 test_cog_review_strategy。
    set(COG_GRAD_TEST_SRCS "")
    if(NOT TARGET airy_cognition)
        set(COG_GRAD_TEST_SRCS
            "${COG_CONTRACT_DIR}/../src/cognition/think/plan_model.c")
    endif()
    add_executable(test_grad_strategy tests/unit/test_grad.c
        tests/unit/test_grad_verifiers.c
        tests/unit/test_grad_coordinator.c
        tests/unit/test_grad_arbiter.c
        tests/unit/test_grad_misc.c
        ${COG_GRAD_TEST_SRCS})
    target_compile_definitions(test_grad_strategy PRIVATE AIRY_COMPLIANCE_IMPL)
    if(_cog_cjson_libs)
        target_compile_definitions(test_grad_strategy PRIVATE AIRY_HAS_CJSON=1)
    endif()
    target_include_directories(test_grad_strategy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
        ${_cog_cjson_inc}
    )
    target_link_libraries(test_grad_strategy PRIVATE airy_cognition_strategy)
    if(TARGET airy_cognition)
        target_link_libraries(test_grad_strategy PRIVATE airy_cognition)
    endif()
    if(_cog_cjson_libs)
        target_link_libraries(test_grad_strategy PRIVATE ${_cog_cjson_libs})
    endif()
    if(NOT MSVC)
        target_link_libraries(test_grad_strategy PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_grad_tests COMMAND test_grad_strategy)

    # MC 元认知策略载荷测试（双模式）：被测符号全部为 airy_mc_* 载荷
    # AIRY_API（0.1.19 M5-4 §268 自 atoms 迁入），embedded/standalone 均
    # 仅链本库；载荷内部头（metacognition.h/thinking_chain.h）由
    # src/foundation 解析，契约头由 COG_CONTRACT_DIR 解析。
    add_executable(test_metacognition_calibration tests/unit/test_metacognition_calibration.c)
    target_compile_definitions(test_metacognition_calibration PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_metacognition_calibration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/foundation
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
    )
    target_link_libraries(test_metacognition_calibration PRIVATE airy_cognition_strategy)
    if(NOT MSVC)
        target_link_libraries(test_metacognition_calibration PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_mc_calibration_tests COMMAND test_metacognition_calibration)

    # 意图解析策略载荷测试（双模式）：被测符号全部为 cognition.h 公开
    # API（airy_intent_parser_*，0.1.19 M5-4 §269 自 atoms 迁入）；
    # 载荷机制侧符号依赖（id_utils/gccp_goal）在 embedded 由
    # airy_coreloopthree 承接，standalone 直编其实现单元（镜像
    # COG_GCCP_TEST_SRCS 范式）；零载荷内部头依赖，契约头由
    # COG_CONTRACT_DIR 解析。
    set(COG_INTENT_TEST_SRCS "")
    if(NOT TARGET airy_coreloopthree)
        set(COG_INTENT_TEST_SRCS
            "${COG_CONTRACT_DIR}/../src/utils/id_utils.c"
            "${COG_CONTRACT_DIR}/../src/cognition/gccp/gccp_goal.c")
    endif()
    add_executable(test_intent_parser tests/unit/test_intent_parser.c ${COG_INTENT_TEST_SRCS})
    target_compile_definitions(test_intent_parser PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_intent_parser PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
        ${COG_COMMONS_DIR}/utils/uuid
    )
    target_link_libraries(test_intent_parser PRIVATE airy_cognition_strategy)
    if(TARGET airy_coreloopthree)
        target_link_libraries(test_intent_parser PRIVATE airy_coreloopthree)
    endif()
    if(NOT MSVC)
        target_link_libraries(test_intent_parser PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_intent_parser_tests COMMAND test_intent_parser)

    # 规划策略载荷测试（双模式）：被测符号为 reactive/reflective 工厂与
    # payload_registry plan 注表（0.1.19 M5-4 §271 自 atoms 迁入）；
    # are_ops_get_plan 存储面在 embedded 由 airy_cognition（ops_injection）
    # 承接，standalone 由本库 COG_PLAN_BASE_SRC 直编；计划数据模型
    # （airy_task_plan_free，plan_model.c）在 embedded 由 airy_cognition
    # 目标提供，standalone 直编其实现单元（镜像 COG_GRAD_TEST_SRCS 范式）；
    # reactive 规则路径与 reflective fallback 计划均不依赖 LLM（llm 传
    # NULL）。
    set(COG_PLAN_TEST_SRCS "")
    if(NOT TARGET airy_cognition)
        set(COG_PLAN_TEST_SRCS
            "${COG_CONTRACT_DIR}/../src/cognition/think/plan_model.c"
            # standalone 机制接缝缺席态闭合单元：payload_registry 的 MC/TC
            # 载荷 memory 槽位（tc_memory.c）引用机制接口符号，机制核缺席
            # 形态下由本单元闭合（防御语义镜像 engine.c 判空早退，非桩）；
            # embedded 由 airy_coreloopthree 真实现承接，不参与编译。
            tests/standalone/mech_memory_absent.c)
    endif()
    add_executable(test_plan_reactive tests/unit/test_plan_reactive.c ${COG_PLAN_TEST_SRCS})
    target_compile_definitions(test_plan_reactive PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_plan_reactive PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
    )
    target_link_libraries(test_plan_reactive PRIVATE airy_cognition_strategy)
    if(TARGET airy_coreloopthree)
        target_link_libraries(test_plan_reactive PRIVATE airy_coreloopthree)
    endif()
    if(NOT MSVC)
        target_link_libraries(test_plan_reactive PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_plan_reactive_tests COMMAND test_plan_reactive)

    # 认知进化策略载荷测试（双模式）：被测符号全部为 cog_evolution_*
    # 载荷 AIRY_API（0.1.19 M5-4 §270 自 atoms 迁入），零机制侧符号
    # 依赖，embedded/standalone 均仅链本库；契约头自足（仅 std 头）。
    add_executable(test_cognitive_evolution tests/unit/test_cognitive_evolution.c)
    target_compile_definitions(test_cognitive_evolution PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_cognitive_evolution PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${COG_CONTRACT_DIR}
        ${AIRY_COREKERN_INCLUDE_DIR}
    )
    target_link_libraries(test_cognitive_evolution PRIVATE airy_cognition_strategy)
    if(NOT MSVC)
        target_link_libraries(test_cognitive_evolution PRIVATE pthread dl m)
    endif()
    add_test(NAME cognition_evolution_tests COMMAND test_cognitive_evolution)

    # 机制核集成测试（embedded-only）：被测符号横跨机制核（airy_cognition_*/
    # airy_hook_*/airy_intent_parser_*/airy_plan_reactive_create）与 MC/TC
    # 载荷（airy_tc_working_memory_*），仅在 agentrt 超级构建（机制核
    # airy_coreloopthree 目标存在）时挂载；standalone 形态无机制核，不建。
    # 机制公开头（cognition.h/memory.h）由 COG_CONTRACT_DIR 解析，载荷
    # 契约（plan_strategy.h）由本库 include 解析，载荷内部头由 src、
    # src/foundation 解析。airy_coreloopthree 的 PUBLIC
    # 闭包已收口 atoms 域 + 零依赖 ops 归档（daemons 断链），无 svc 系
    # 传递依赖。
    if(TARGET airy_coreloopthree)
        foreach(COG_MECH_TEST test_cognition;test_cognition_e2e;test_hook_integration;test_reactive_planner;test_thread_safety)
            set(COG_MECH_EXTRA "")
            if(COG_MECH_TEST STREQUAL test_thread_safety)
                set(COG_MECH_EXTRA
                    tests/unit/test_thread_safety_int0201.c
                    tests/unit/test_thread_safety_int0202.c)
            endif()
            add_executable(${COG_MECH_TEST} tests/unit/${COG_MECH_TEST}.c ${COG_MECH_EXTRA})
            target_compile_definitions(${COG_MECH_TEST} PRIVATE AIRY_COMPLIANCE_IMPL)
            target_include_directories(${COG_MECH_TEST} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/src/foundation
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${COG_CONTRACT_DIR}
                "${COG_CONTRACT_DIR}/../src/cognition/think"
                ${AIRY_COREKERN_INCLUDE_DIR}
            )
            target_link_libraries(${COG_MECH_TEST} PRIVATE
                airy_cognition_strategy
                airy_coreloopthree)
            if(MSVC)
                target_compile_options(${COG_MECH_TEST} PRIVATE "/FIairy_memory.h")
            else()
                target_compile_options(${COG_MECH_TEST} PRIVATE "SHELL:-include airy_memory.h")
                target_link_libraries(${COG_MECH_TEST} PRIVATE pthread dl m)
            endif()
            add_test(NAME cognition_${COG_MECH_TEST} COMMAND ${COG_MECH_TEST})
        endforeach()
    endif()
endif()

install(TARGETS airy_cognition_strategy
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib/static
)

install(FILES include/dispatch_strategy.h include/plan_strategy.h include/coord_strategy.h
    include/cog_review_strategy.h include/gccp_strategy.h include/grad_strategy.h
    include/payload_registry.h include/cognitive_evolution.h
    DESTINATION include/agentrt/cognition
)
