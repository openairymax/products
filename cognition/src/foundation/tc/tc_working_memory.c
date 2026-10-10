// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tc_working_memory.c
 * @brief Thinking-chain Working Memory domain: short-term key-value cache with LRU eviction.
 */

#include "tc_internal.h"

/* ============================================================================
 * Working Memory implementation
 * ============================================================================ */

airy_err_t airy_tc_working_memory_create(size_t capacity, airy_working_memory_t **out_mem)
{

    if (!out_mem) {
        AIRY_LOG_ERROR("airy_tc_working_memory_create: NULL out_mem parameter");
        return AIRY_EINVAL;
    }

    size_t cap = (capacity > 0) ? capacity : TC_WORKING_MEM_CAPACITY;
    airy_working_memory_t *mem =
        (airy_working_memory_t *)AIRY_CALLOC(1, sizeof(airy_working_memory_t));
    if (!mem) {
        AIRY_LOG_ERROR(
            "airy_tc_working_memory_create: allocation failed for working memory (capacity=%zu)",
            cap);
        return AIRY_ENOMEM;
    }

    mem->entries = (struct wm_entry *)AIRY_CALLOC(cap, sizeof(struct wm_entry));
    if (!mem->entries) {
        AIRY_LOG_ERROR("airy_tc_working_memory_create: entries allocation failed (capacity=%zu)",
                       cap);
        AIRY_FREE(mem);
        return AIRY_ENOMEM;
    }
    mem->lru_order = (uint32_t *)AIRY_CALLOC(cap, sizeof(uint32_t));
    if (!mem->lru_order) {
        AIRY_LOG_ERROR("airy_tc_working_memory_create: lru_order allocation failed (capacity=%zu)",
                       cap);
        AIRY_FREE(mem->entries);
        AIRY_FREE(mem);
        return AIRY_ENOMEM;
    }

    mem->capacity = cap;
    mem->count = 0;
    mem->lru_index = 0;
    mem->hits = 0;
    mem->misses = 0;
    mem->evictions = 0;

    *out_mem = mem;
    return AIRY_SUCCESS;
}

void airy_tc_working_memory_destroy(airy_working_memory_t *mem)
{
    if (!mem)
        return;
    for (size_t i = 0; i < mem->count; i++) {
        if (mem->entries[i].key)
            AIRY_FREE(mem->entries[i].key);
        if (mem->entries[i].value)
            AIRY_FREE(mem->entries[i].value);
        if (mem->entries[i].type)
            AIRY_FREE(mem->entries[i].type);
    }
    AIRY_FREE(mem->entries);
    AIRY_FREE(mem->lru_order);
    AIRY_FREE(mem);
}

static size_t wm_find_key(airy_working_memory_t *mem, const char *key)
{
    for (size_t i = 0; i < mem->count; i++) {
        if (mem->entries[i].key && strcmp(mem->entries[i].key, key) == 0)
            return i;
    }
    return (size_t)-1;
}

static void wm_update_lru(airy_working_memory_t *mem, size_t idx)
{
    if (idx >= mem->count)
        return;
    mem->entries[idx].last_accessed_ns = tc_time_now_ns();
    mem->entries[idx].access_count++;
    mem->lru_order[mem->lru_index++] = (uint32_t)idx;
    if (mem->lru_index >= mem->capacity)
        mem->lru_index = 0;
}

static void wm_evict_one(airy_working_memory_t *mem)
{
    if (mem->count == 0)
        return;

    size_t victim = (size_t)-1;
    uint64_t oldest_access = UINT64_MAX;

    for (size_t i = 0; i < mem->count; i++) {
        if (!mem->entries[i].pinned && mem->entries[i].last_accessed_ns < oldest_access) {
            oldest_access = mem->entries[i].last_accessed_ns;
            victim = i;
        }
    }

    if (victim != (size_t)-1) {
        AIRY_FREE(mem->entries[victim].key);
        AIRY_FREE(mem->entries[victim].value);
        if (mem->entries[victim].type)
            AIRY_FREE(mem->entries[victim].type);

        mem->entries[victim] = mem->entries[mem->count - 1];
        mem->count--;
        mem->evictions++;
    }
}

airy_err_t airy_tc_working_memory_store(airy_working_memory_t *mem, const char *key,
                                        const void *value, size_t value_size, const char *type,
                                        int pin)
{

    if (!mem || !key || !value || value_size == 0) {
        AIRY_LOG_ERROR("airy_tc_working_memory_store: NULL/invalid params (mem=%p key=%p value=%p "
                       "value_size=%zu)",
                       (void *)mem, (void *)key, (void *)value, value_size);
        return AIRY_EINVAL;
    }

    size_t existing = wm_find_key(mem, key);
    if (existing != (size_t)-1) {
        void *new_val = AIRY_MALLOC(value_size);
        if (!new_val) {
            AIRY_LOG_ERROR("airy_tc_working_memory_store: value update allocation failed for "
                           "existing key (key=%s value_size=%zu)",
                           key, value_size);
            return AIRY_ENOMEM;
        }
        __builtin_memcpy(new_val, value, value_size);
        AIRY_FREE(mem->entries[existing].value);
        mem->entries[existing].value = new_val;
        mem->entries[existing].value_size = value_size;
        if (type) {
            char *t = AIRY_STRDUP(type);
            if (t) {
                AIRY_FREE(mem->entries[existing].type);
                mem->entries[existing].type = t;
            }
        }
        mem->entries[existing].pinned = pin;
        wm_update_lru(mem, existing);
        return AIRY_SUCCESS;
    }

    if (mem->count >= mem->capacity && !pin) {
        wm_evict_one(mem);
    }
    if (mem->count >= mem->capacity) {
        AIRY_LOG_WARN(
            "airy_tc_working_memory_store: capacity exhausted (count=%zu capacity=%zu key=%s)",
            mem->count, mem->capacity, key);
        return AIRY_ENOMEM;
    }

    struct wm_entry *e = &mem->entries[mem->count];
    e->key = AIRY_STRDUP(key);
    e->value = AIRY_MALLOC(value_size);
    if (!e->key || !e->value) {
        AIRY_LOG_ERROR(
            "airy_tc_working_memory_store: key/value allocation failed (key=%s value_size=%zu)",
            key, value_size);
        AIRY_FREE(e->key);
        AIRY_FREE(e->value);
        e->key = NULL;
        e->value = NULL;
        return AIRY_ENOMEM;
    }
    __builtin_memcpy(e->value, value, value_size);
    e->value_size = value_size;
    e->type = type ? AIRY_STRDUP(type) : NULL;
    e->created_ns = tc_time_now_ns();
    e->last_accessed_ns = e->created_ns;
    e->access_count = 1;
    e->pinned = pin;

    wm_update_lru(mem, mem->count);
    mem->count++;
    return AIRY_SUCCESS;
}

airy_err_t airy_tc_working_memory_retrieve(airy_working_memory_t *mem, const char *key,
                                           void **out_value, size_t *out_size)
{

    if (!mem || !key || !out_value) {
        AIRY_LOG_ERROR("airy_tc_working_memory_retrieve: NULL params (mem=%p key=%p out_value=%p)",
                       (void *)mem, (void *)key, (void *)out_value);
        return AIRY_EINVAL;
    }

    size_t idx = wm_find_key(mem, key);
    if (idx == (size_t)-1) {
        mem->misses++;
        AIRY_LOG_WARN("airy_tc_working_memory_retrieve: cache miss (key=%s misses=%llu)", key,
                      (unsigned long long)mem->misses);
        return AIRY_ENOENT;
    }

    mem->hits++;
    wm_update_lru(mem, idx);
    *out_value = mem->entries[idx].value;
    if (out_size)
        *out_size = mem->entries[idx].value_size;
    return AIRY_SUCCESS;
}

airy_err_t airy_tc_working_memory_remove(airy_working_memory_t *mem, const char *key)
{

    if (!mem || !key) {
        AIRY_LOG_ERROR("airy_tc_working_memory_remove: NULL params (mem=%p key=%p)", (void *)mem,
                       (void *)key);
        return AIRY_EINVAL;
    }

    size_t idx = wm_find_key(mem, key);
    if (idx == (size_t)-1) {
        AIRY_LOG_WARN("airy_tc_working_memory_remove: key not found (key=%s)", key);
        return AIRY_ENOENT;
    }

    AIRY_FREE(mem->entries[idx].key);
    AIRY_FREE(mem->entries[idx].value);
    if (mem->entries[idx].type)
        AIRY_FREE(mem->entries[idx].type);

    mem->entries[idx] = mem->entries[mem->count - 1];
    mem->count--;
    return AIRY_SUCCESS;
}

void airy_tc_working_memory_clear_unpinned(airy_working_memory_t *mem)
{
    if (!mem)
        return;
    size_t i = 0;
    while (i < mem->count) {
        if (!mem->entries[i].pinned) {
            AIRY_FREE(mem->entries[i].key);
            AIRY_FREE(mem->entries[i].value);
            if (mem->entries[i].type)
                AIRY_FREE(mem->entries[i].type);
            mem->entries[i] = mem->entries[mem->count - 1];
            mem->count--;
        } else {
            i++;
        }
    }
}
airy_err_t airy_tc_wm_set_priority(airy_working_memory_t *wm, const char *key, float priority)
{
    if (!wm || !key) {
        AIRY_LOG_ERROR("airy_tc_wm_set_priority: NULL params (wm=%p key=%p)", (void *)wm,
                       (void *)key);
        return AIRY_EINVAL;
    }
    if (priority < 0.0f)
        priority = 0.0f;
    if (priority > 1.0f)
        priority = 1.0f;

    for (size_t i = 0; i < wm->count; i++) {
        if (strncmp(wm->entries[i].key, key, 255) == 0) {
            if (priority > 0.7f)
                wm->entries[i].pinned = 1;
            wm_update_lru(wm, i);
            return AIRY_SUCCESS;
        }
    }
    AIRY_LOG_WARN("airy_tc_wm_set_priority: key not found (key=%s)", key);
    return AIRY_ENOENT;
}
