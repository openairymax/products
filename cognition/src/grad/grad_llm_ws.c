// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file grad_llm_ws.c
 * @brief GRAD 决策链工作区与文件持久化域
 *
 * mkdir -p 语义的跨平台目录创建、工作区路径构建、文件写入（目录自动
 * 创建），以及决策链 JSONL 日志（trace/chain.jsonl）的开/写/关。
 */

#include "grad_internal.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>

static int grad_mkdir_one(const char *path)
{
    return _mkdir(path);
}
#else
#include <sys/stat.h>
#include <sys/types.h>
static int grad_mkdir_one(const char *path)
{
    return mkdir(path, 0755);
}
#endif

/**
 * @brief Recursively create directories (mkdir -p semantics, cross-platform).
 *
 * Creates each level of the path in turn, silently skipping existing
 * levels.
 * @param path [in] Target directory path
 * @return 0 success (including already-existing); -1 failure
 */
int grad_ws_mkdir(const char *path)
{
    if (!path || !path[0])
        return -1;

    size_t len = strlen(path);
    char *buf = (char *)AIRY_MALLOC(len + 1);
    if (!buf)
        return -1;
    AIRY_MEMCPY(buf, path, len + 1);

    int result = 0;
    for (size_t i = 0; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\\' || buf[i] == '\0') {
            if (i == 0)
                continue;
            char saved = buf[i];
            buf[i] = '\0';
            if (buf[0] != '\0') {
                if (grad_mkdir_one(buf) != 0) {
#ifdef _WIN32
                    if (_access(buf, 0) != 0) {
#else
                    struct stat st;
                    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
#endif
                        result = -1;
                    }
                }
            }
            buf[i] = saved;
        }
    }
    AIRY_FREE(buf);
    return result;
}

/**
 * @brief Build the workspace directory path: <workspace_root>/<plan_id>/
 * Returns a heap-allocated path (OWNER). Returns NULL when workspace_root
 * is empty (no persistence).
 */
char *grad_ws_dir(const grad_llm_ctx_t *ctx)
{
    if (!ctx || !ctx->workspace_root || !ctx->plan_id)
        return NULL;
    size_t cap = strlen(ctx->workspace_root) + strlen(ctx->plan_id) + 8;
    char *dir = (char *)AIRY_MALLOC(cap);
    if (!dir)
        return NULL;
    snprintf(dir, cap, "%s/%s", ctx->workspace_root, ctx->plan_id);
    return dir;
}

/**
 * @brief Write a file (mkdir -p the directory), returns 0 on success.
 */
int grad_ws_write_file(const char *dir, const char *sub, const char *name, const char *data,
                       size_t data_len)
{
    if (!dir || !name || !data)
        return -1;
    char path[1024];
    if (sub && sub[0])
        snprintf(path, sizeof(path), "%s/%s/%s", dir, sub, name);
    else
        snprintf(path, sizeof(path), "%s/%s", dir, name);

    if (sub && sub[0]) {
        char subpath[1024];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir, sub);
        grad_ws_mkdir(subpath);
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    size_t w = fwrite(data, 1, data_len, f);
    fclose(f);
    return (w == data_len) ? 0 : -1;
}

airy_err_t grad_llm_trace_open(grad_llm_ctx_t *ctx)
{
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->workspace_root || !ctx->plan_id)
        return AIRY_SUCCESS;
    if (ctx->chain_log)
        return AIRY_SUCCESS;

    char *dir = grad_ws_dir(ctx);
    if (!dir)
        return AIRY_ENOMEM;
    grad_ws_mkdir(dir);
    char path[1024];
    snprintf(path, sizeof(path), "%s/trace", dir);
    grad_ws_mkdir(path);
    snprintf(path, sizeof(path), "%s/trace/chain.jsonl", dir);
    FILE *f = fopen(path, "a");
    AIRY_FREE(dir);
    if (!f)
        return AIRY_ESERVICE;
    ctx->chain_log = (void *)f;
    return AIRY_SUCCESS;
}

airy_err_t grad_llm_trace_append(grad_llm_ctx_t *ctx, const char *event, const char *json)
{
    if (!ctx || !event || !json)
        return AIRY_EINVAL;
    FILE *f = (FILE *)ctx->chain_log;
    if (!f)
        return AIRY_SUCCESS;

    fprintf(f, "{\"event\":\"%s\",\"ts\":%llu,\"data\":%s}\n", event,
            (unsigned long long)airy_time_monotonic_ns(), json);
    fflush(f);
    return AIRY_SUCCESS;
}

void grad_llm_trace_close(grad_llm_ctx_t *ctx)
{
    if (!ctx)
        return;
    FILE *f = (FILE *)ctx->chain_log;
    if (f) {
        fclose(f);
        ctx->chain_log = NULL;
    }
}
