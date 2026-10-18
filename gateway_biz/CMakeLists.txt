# ============================================================================
# Gateway Biz Product - gateway_d 业务处理面（嵌入式产品件）
# Copyright (C) 2026 SPHARX. All Rights Reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#
# 0.1.19 刀H：本产品件自 agentrt/gateway/src/biz/ 迁出。业务处理器依赖
# daemon 公共层（svc_common / daemon_security）与 gateway_d 协议适配头，
# 不属于纯 gateway 机制核，故以产品件形态寄宿于 products/，由 gateway_d
# 装配时挂入。
#
# 宿主契约（embedding-only）：
#   AIRY_GATEWAY_BIZ_AGENTRT_ROOT  必填，agentrt 源码根
#   AIRY_GATEWAY_BIZ_GEN_DIR       必填，gateway_policy_gen 产物目录
#   目标 gateway_policy_gen / svc_common / airy_protocols 必须已存在
#
# 缺席语义：宿主探测不到本目录即不挂载 gateway_d，不产生任何桩实现。
# ============================================================================

foreach(_var AIRY_GATEWAY_BIZ_AGENTRT_ROOT AIRY_GATEWAY_BIZ_GEN_DIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR
            "gateway_biz: 宿主必须先设置 ${_var}（本产品件仅支持嵌入式装配）")
    endif()
endforeach()

foreach(_tgt gateway_policy_gen svc_common airy_protocols)
    if(NOT TARGET ${_tgt})
        message(FATAL_ERROR "gateway_biz: 缺少宿主目标 ${_tgt}")
    endif()
endforeach()

set(_rt "${AIRY_GATEWAY_BIZ_AGENTRT_ROOT}")
set(_gw "${_rt}/gateway")

set(GBIZ_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_biz_agent.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_biz_backend.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_biz_forward.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_biz_hall.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_biz_svcdispatch.c
    # V13.2 按需激活单向通知（supervisor.activate fire-and-forget）
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_biz_supactivate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_biz_tools.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_business_handler.c
    # 统一能力注册表（cap_key 单一权威源）
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_cap_registry.c
    # PEP：裁定缓存（epoch 失效键），gateway 热路径
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_pep_cache.c
)

add_library(airy_gateway_biz STATIC ${GBIZ_SOURCES})
add_dependencies(airy_gateway_biz gateway_policy_gen)

target_include_directories(airy_gateway_biz PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${_gw}/include
    ${_gw}/src
    ${_gw}/src/gateway
    ${_gw}/src/utils
    ${_gw}/src/utils/syscall
    ${_rt}/atoms/syscall/include
    ${_rt}/atoms/corekern/include
    ${_rt}/daemons/common/include
    # gateway_biz_internal.h 传递包含 gateway_mcp_server.h；
    # 协议适配实现待 0.2.x 下沉 protocols/core 后解除此依赖。
    ${_rt}/daemons/gateway_d/include
    ${_rt}/daemons/gateway_d/src/protocol
    ${_rt}/commons
    ${_rt}/commons/include
    ${_rt}/commons/platform/include
    ${_rt}/commons/utils/include
    ${_rt}/commons/utils/error
    ${_rt}/commons/utils/logging
    ${_rt}/commons/utils/observability
    ${_rt}/commons/utils/config_unified
    ${_rt}/commons/utils/memory
    ${_rt}/commons/utils/string
    ${_rt}/commons/utils/sync
    ${_rt}/commons/utils/cache
    ${_rt}/commons/utils/compat
    ${_rt}/commons/utils/network
    ${_rt}/commons/utils/ipc
    ${_rt}/commons/utils/hall
    ${_rt}/commons/utils/types
    ${_rt}/protocols/include
    ${_rt}/protocols/common/include
    ${_rt}/protocols/standards/mcp/include
    ${_rt}/commons/include/third_party
)

target_include_directories(airy_gateway_biz PRIVATE
    ${AIRY_GATEWAY_BIZ_GEN_DIR}
    ${CJSON_INCLUDE_DIRS}
)

target_link_libraries(airy_gateway_biz PUBLIC svc_common airy_protocols)
