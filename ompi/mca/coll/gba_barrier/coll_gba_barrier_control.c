/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2024      Global Barrier Accelerator Implementation
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 * GBA Barrier Control Plane - Device and register access
 *
 * This module provides the control plane interface for communicating
 * with the Global Barrier Accelerator hardware. It handles:
 * - Device initialization via MMIO mapping
 * - Register read/write operations
 * - Barrier group allocation and configuration
 */

#include "ompi_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "opal/util/output.h"
#include "opal/mca/threads/mutex.h"
#include "ompi/constants.h"

#include "coll_gba_barrier.h"

/** Total register space size (32 groups x 4KB each) */
#define GBA_REG_SPACE_SIZE      (GBA_MAX_GROUPS * GBA_GROUP_REG_SIZE)

/** Register space stride within a group */
#define GBA_GROUP_REG_STRIDE    0x08

int gba_device_init(gba_device_t *device, const char *dev_path)
{
    if (NULL == device || NULL == dev_path) {
        return OMPI_ERR_BAD_PARAM;
    }

    memset(device, 0, sizeof(*device));
    OBJ_CONSTRUCT(&device->lock, opal_mutex_t);

    /* Open GBA device */
    device->device_fd = open(dev_path, O_RDWR);
    if (device->device_fd < 0) {
        opal_output_verbose(5, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: failed to open %s: %s",
                            dev_path, strerror(errno));
        OBJ_DESTRUCT(&device->lock);
        return OMPI_ERR_NOT_AVAILABLE;
    }

    /* Map GBA register space */
    device->base_addr = mmap(NULL, GBA_REG_SPACE_SIZE,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, device->device_fd, 0);
    if (device->base_addr == MAP_FAILED) {
        opal_output_verbose(5, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: mmap failed: %s",
                            strerror(errno));
        close(device->device_fd);
        device->device_fd = -1;
        OBJ_DESTRUCT(&device->lock);
        return OMPI_ERR_NOT_AVAILABLE;
    }

    device->num_groups = GBA_MAX_GROUPS;
    device->group_alloc_mask = 0;

    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: device %s initialized, "
                        "reg_space=%p, size=%d",
                        dev_path, device->base_addr, GBA_REG_SPACE_SIZE);

    return OMPI_SUCCESS;
}

int gba_device_fini(gba_device_t *device)
{
    if (NULL == device) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&device->lock);

    if (device->base_addr != NULL && device->base_addr != MAP_FAILED) {
        munmap(device->base_addr, GBA_REG_SPACE_SIZE);
        device->base_addr = NULL;
    }

    if (device->device_fd >= 0) {
        close(device->device_fd);
        device->device_fd = -1;
    }

    device->num_groups = 0;
    device->group_alloc_mask = 0;

    OPAL_THREAD_UNLOCK(&device->lock);
    OBJ_DESTRUCT(&device->lock);

    return OMPI_SUCCESS;
}

int gba_reg_read(gba_device_t *device, uint32_t group_id,
                 uint32_t offset, uint64_t *value)
{
    volatile uint64_t *reg_ptr;
    uint64_t addr;

    if (NULL == device || NULL == value) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (group_id >= GBA_MAX_GROUPS) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (device->base_addr == NULL || device->base_addr == MAP_FAILED) {
        return OMPI_ERR_NOT_AVAILABLE;
    }

    addr = GBA_GROUP_REG_BASE(group_id) + offset;
    reg_ptr = (volatile uint64_t *)((char *)device->base_addr + addr);

    /* Memory barrier before read */
    opal_atomic_rmb();
    *value = *reg_ptr;

    return OMPI_SUCCESS;
}

int gba_reg_write(gba_device_t *device, uint32_t group_id,
                  uint32_t offset, uint64_t value)
{
    volatile uint64_t *reg_ptr;
    uint64_t addr;

    if (NULL == device) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (group_id >= GBA_MAX_GROUPS) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (device->base_addr == NULL || device->base_addr == MAP_FAILED) {
        return OMPI_ERR_NOT_AVAILABLE;
    }

    addr = GBA_GROUP_REG_BASE(group_id) + offset;
    reg_ptr = (volatile uint64_t *)((char *)device->base_addr + addr);

    *reg_ptr = value;
    /* Memory barrier after write */
    opal_atomic_wmb();

    return OMPI_SUCCESS;
}

int gba_allocate_group(gba_device_t *device, uint32_t *group_id)
{
    int i;

    if (NULL == device || NULL == group_id) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&device->lock);

    /* Find first available group (32 groups max) */
    for (i = 0; i < GBA_MAX_GROUPS; i++) {
        if (!(device->group_alloc_mask & (1U << i))) {
            device->group_alloc_mask |= (1U << i);
            *group_id = i;
            OPAL_THREAD_UNLOCK(&device->lock);

            opal_output_verbose(20, ompi_coll_base_framework.framework_output,
                                "coll:gba_barrier: allocated group %u", i);
            return OMPI_SUCCESS;
        }
    }

    OPAL_THREAD_UNLOCK(&device->lock);

    opal_output_verbose(5, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: no available groups (all 32 in use)");
    return OMPI_ERR_OUT_OF_RESOURCE;
}

int gba_free_group(gba_device_t *device, uint32_t group_id)
{
    if (NULL == device) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (group_id >= GBA_MAX_GROUPS) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&device->lock);

    /* Reset group state in hardware */
    gba_reg_write(device, group_id, GBA_REG_CONTROL, GBA_CTRL_RESET);

    /* Clear allocation bit */
    device->group_alloc_mask &= ~(1U << group_id);

    OPAL_THREAD_UNLOCK(&device->lock);

    opal_output_verbose(20, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: freed group %u", group_id);

    return OMPI_SUCCESS;
}

int gba_configure_group(gba_device_t *device, gba_group_config_t *config)
{
    int ret;
    int i;
    uint64_t ctrl_val;

    if (NULL == device || NULL == config) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&device->lock);

    /* Step 1: Reset the group */
    ret = gba_reg_write(device, config->group_id, GBA_REG_CONTROL, 0);
    if (OMPI_SUCCESS != ret) {
        goto unlock;
    }

    ret = gba_reg_write(device, config->group_id, GBA_REG_CONTROL,
                        GBA_CTRL_RESET);
    if (OMPI_SUCCESS != ret) {
        goto unlock;
    }

    /* Step 2: Configure group ID */
    ret = gba_reg_write(device, config->group_id, GBA_REG_GROUP_ID,
                        config->group_id);
    if (OMPI_SUCCESS != ret) {
        goto unlock;
    }

    /* Step 3: Configure member count */
    ret = gba_reg_write(device, config->group_id, GBA_REG_MEMBER_COUNT,
                        config->member_count);
    if (OMPI_SUCCESS != ret) {
        goto unlock;
    }

    /* Step 4: Configure member mask (12 x 64-bit words for 708 bits) */
    for (i = 0; i < GBA_MEMBER_MASK_WORDS; i++) {
        ret = gba_reg_write(device, config->group_id,
                            GBA_REG_MEMBER_MASK_BASE + (i * 8),
                            config->member_mask[i]);
        if (OMPI_SUCCESS != ret) {
            goto unlock;
        }
    }

    /* Step 5: Enable and arm the group */
    ctrl_val = GBA_CTRL_ENABLE | GBA_CTRL_ARM;
    ret = gba_reg_write(device, config->group_id, GBA_REG_CONTROL, ctrl_val);

unlock:
    OPAL_THREAD_UNLOCK(&device->lock);

    if (OMPI_SUCCESS == ret) {
        opal_output_verbose(20, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: configured group %u "
                            "(members=%u, local_id=%u)",
                            config->group_id, config->member_count,
                            config->local_member_id);
    }

    return ret;
}

/*
 * ============================================================================
 * DMA Context Functions
 * ============================================================================
 */

int gba_dma_init(gba_dma_context_t *ctx, gba_device_t *device)
{
    if (NULL == ctx) {
        return OMPI_ERR_BAD_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    OBJ_CONSTRUCT(&ctx->lock, opal_mutex_t);

    ctx->dma_fd = -1;
    ctx->dma_handle = NULL;

    /*
     * TODO: Platform-specific DMA initialization
     *
     * Linux VFIO example:
     *   ctx->dma_fd = open("/dev/vfio/vfio", O_RDWR);
     *
     * Intel VT-d / ARM SMMU:
     *   Use appropriate kernel interfaces
     */

    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: DMA context initialized");

    return OMPI_SUCCESS;
}

int gba_dma_fini(gba_dma_context_t *ctx)
{
    if (NULL == ctx) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&ctx->lock);

    if (ctx->dma_fd >= 0) {
        close(ctx->dma_fd);
        ctx->dma_fd = -1;
    }

    OPAL_THREAD_UNLOCK(&ctx->lock);
    OBJ_DESTRUCT(&ctx->lock);

    return OMPI_SUCCESS;
}

int gba_dma_map(gba_dma_context_t *ctx, void *vaddr, size_t size,
                uint64_t *dma_addr)
{
    if (NULL == ctx || NULL == vaddr || NULL == dma_addr || 0 == size) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&ctx->lock);

    /*
     * TODO: Platform-specific DMA mapping
     *
     * For now, use identity mapping (VA == DMA address).
     * In production, this should use:
     * - VFIO DMA mapping for user-space
     * - Kernel DMA API for kernel drivers
     */

    *dma_addr = (uint64_t)(uintptr_t)vaddr;

    OPAL_THREAD_UNLOCK(&ctx->lock);

    opal_output_verbose(20, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: DMA mapped vaddr=%p -> dma=0x%lx",
                        vaddr, (unsigned long)*dma_addr);

    return OMPI_SUCCESS;
}

int gba_dma_unmap(gba_dma_context_t *ctx, uint64_t dma_addr)
{
    if (NULL == ctx) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&ctx->lock);

    /*
     * TODO: Platform-specific DMA unmapping
     */

    OPAL_THREAD_UNLOCK(&ctx->lock);

    opal_output_verbose(20, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: DMA unmapped dma=0x%lx",
                        (unsigned long)dma_addr);

    return OMPI_SUCCESS;
}

/*
 * ============================================================================
 * Local State Functions
 * ============================================================================
 */

int gba_local_state_init(gba_local_state_t *state, gba_dma_context_t *ctx)
{
    int ret;
    size_t page_size;

    if (NULL == state) {
        return OMPI_ERR_BAD_PARAM;
    }

    memset(state, 0, sizeof(*state));

    /* Allocate a page for the release flag */
    page_size = sysconf(_SC_PAGESIZE);
    state->flag_size = page_size;

    state->flag_memory = mmap(NULL, state->flag_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (state->flag_memory == MAP_FAILED) {
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    memset(state->flag_memory, 0, state->flag_size);

    /* Set up release flag pointer (cache-line aligned) */
    state->release_flag = (volatile uint64_t *)state->flag_memory;
    state->expected_seq = 1;

    /* Map for DMA access by GBA */
    if (ctx != NULL) {
        ret = gba_dma_map(ctx, state->flag_memory, state->flag_size,
                          &state->dma_addr);
        if (OMPI_SUCCESS != ret) {
            munmap(state->flag_memory, state->flag_size);
            state->flag_memory = NULL;
            return ret;
        }
    } else {
        state->dma_addr = (uint64_t)(uintptr_t)state->flag_memory;
    }

    opal_output_verbose(20, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: local state initialized, "
                        "flag=%p, dma_addr=0x%lx",
                        (void *)state->release_flag,
                        (unsigned long)state->dma_addr);

    return OMPI_SUCCESS;
}

int gba_local_state_fini(gba_local_state_t *state, gba_dma_context_t *ctx)
{
    if (NULL == state) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (ctx != NULL && state->dma_addr != 0) {
        gba_dma_unmap(ctx, state->dma_addr);
        state->dma_addr = 0;
    }

    if (state->flag_memory != NULL && state->flag_memory != MAP_FAILED) {
        munmap(state->flag_memory, state->flag_size);
        state->flag_memory = NULL;
    }

    state->release_flag = NULL;

    return OMPI_SUCCESS;
}
