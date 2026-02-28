/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2024      Switch Barrier Accelerator Implementation
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
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

#include "coll_switch_barrier.h"

#define SWITCH_BARRIER_REG_SPACE_SIZE       0x10000
#define SWITCH_BARRIER_GROUP_REG_STRIDE     0x100

static uint64_t switch_barrier_calc_reg_addr(uint32_t group_id, uint32_t reg_offset)
{
    return (group_id * SWITCH_BARRIER_GROUP_REG_STRIDE) + reg_offset;
}

int switch_barrier_control_plane_init(switch_barrier_device_t *device,
                                       const char *dev_path)
{
    if (NULL == device || NULL == dev_path) {
        return OMPI_ERR_BAD_PARAM;
    }

    memset(device, 0, sizeof(*device));
    OBJ_CONSTRUCT(&device->lock, opal_mutex_t);

    device->device_fd = open(dev_path, O_RDWR);
    if (device->device_fd < 0) {
        opal_output(0, "switch_barrier: Failed to open device %s: %s",
                    dev_path, strerror(errno));
        return OMPI_ERR_NOT_AVAILABLE;
    }

    device->base_addr = mmap(NULL, SWITCH_BARRIER_REG_SPACE_SIZE,
                             PROT_READ | PROT_WRITE, MAP_SHARED,
                             device->device_fd, 0);
    if (device->base_addr == MAP_FAILED) {
        opal_output(0, "switch_barrier: Failed to mmap device: %s",
                    strerror(errno));
        close(device->device_fd);
        device->device_fd = -1;
        return OMPI_ERR_NOT_AVAILABLE;
    }

    device->num_groups = SWITCH_BARRIER_MAX_GROUPS;
    device->group_allocation_mask = 0;

    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "switch_barrier: Control plane initialized, device=%s",
                        dev_path);

    return OMPI_SUCCESS;
}

int switch_barrier_control_plane_fini(switch_barrier_device_t *device)
{
    if (NULL == device) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&device->lock);

    if (device->base_addr != NULL && device->base_addr != MAP_FAILED) {
        munmap(device->base_addr, SWITCH_BARRIER_REG_SPACE_SIZE);
        device->base_addr = NULL;
    }

    if (device->device_fd >= 0) {
        close(device->device_fd);
        device->device_fd = -1;
    }

    OPAL_THREAD_UNLOCK(&device->lock);
    OBJ_DESTRUCT(&device->lock);

    return OMPI_SUCCESS;
}

int switch_barrier_reg_read(switch_barrier_device_t *device,
                            uint32_t group_id,
                            uint32_t reg_offset,
                            uint64_t *value)
{
    volatile uint64_t *reg_ptr;
    uint64_t addr;

    if (NULL == device || NULL == value) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (group_id >= (uint32_t)device->num_groups) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (NULL == device->base_addr || device->base_addr == MAP_FAILED) {
        return OMPI_ERR_NOT_AVAILABLE;
    }

    addr = switch_barrier_calc_reg_addr(group_id, reg_offset);
    reg_ptr = (volatile uint64_t *)((char *)device->base_addr + addr);

    opal_atomic_rmb();
    *value = *reg_ptr;

    return OMPI_SUCCESS;
}

int switch_barrier_reg_write(switch_barrier_device_t *device,
                             uint32_t group_id,
                             uint32_t reg_offset,
                             uint64_t value)
{
    volatile uint64_t *reg_ptr;
    uint64_t addr;

    if (NULL == device) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (group_id >= (uint32_t)device->num_groups) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (NULL == device->base_addr || device->base_addr == MAP_FAILED) {
        return OMPI_ERR_NOT_AVAILABLE;
    }

    addr = switch_barrier_calc_reg_addr(group_id, reg_offset);
    reg_ptr = (volatile uint64_t *)((char *)device->base_addr + addr);

    *reg_ptr = value;
    opal_atomic_wmb();

    return OMPI_SUCCESS;
}

int switch_barrier_configure_group(switch_barrier_device_t *device,
                                   switch_barrier_group_config_t *config)
{
    int ret;
    uint64_t ctrl_val;
    int i;

    if (NULL == device || NULL == config) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&device->lock);

    ret = switch_barrier_reg_write(device, config->group_id,
                                   SWITCH_BARRIER_REG_CONTROL, 0);
    if (OMPI_SUCCESS != ret) {
        goto unlock_out;
    }

    ret = switch_barrier_reg_write(device, config->group_id,
                                   SWITCH_BARRIER_REG_CONTROL,
                                   SWITCH_BARRIER_CTRL_RESET);
    if (OMPI_SUCCESS != ret) {
        goto unlock_out;
    }

    ret = switch_barrier_reg_write(device, config->group_id,
                                   SWITCH_BARRIER_REG_GROUP_ID,
                                   config->group_id);
    if (OMPI_SUCCESS != ret) {
        goto unlock_out;
    }

    ret = switch_barrier_reg_write(device, config->group_id,
                                   SWITCH_BARRIER_REG_MEMBER_COUNT,
                                   config->member_count);
    if (OMPI_SUCCESS != ret) {
        goto unlock_out;
    }

    ret = switch_barrier_reg_write(device, config->group_id,
                                   SWITCH_BARRIER_REG_MEMBER_MASK_LO,
                                   config->member_mask[0]);
    if (OMPI_SUCCESS != ret) {
        goto unlock_out;
    }

    ret = switch_barrier_reg_write(device, config->group_id,
                                   SWITCH_BARRIER_REG_MEMBER_MASK_HI,
                                   config->member_mask[1]);
    if (OMPI_SUCCESS != ret) {
        goto unlock_out;
    }

    ret = switch_barrier_reg_write(device, config->group_id,
                                   SWITCH_BARRIER_REG_LOCAL_MEMBER_ID,
                                   config->local_member_id);
    if (OMPI_SUCCESS != ret) {
        goto unlock_out;
    }

    for (i = 0; i < (int)config->member_count && i < SWITCH_BARRIER_MAX_MEMBERS; i++) {
        uint32_t member_addr_offset = SWITCH_BARRIER_REG_ARRIVAL_ADDR +
                                       (i * sizeof(uint64_t));
        ret = switch_barrier_reg_write(device, config->group_id,
                                       member_addr_offset,
                                       config->network_addrs[i]);
        if (OMPI_SUCCESS != ret) {
            goto unlock_out;
        }
    }

    ctrl_val = SWITCH_BARRIER_CTRL_ENABLE | SWITCH_BARRIER_CTRL_ARM;
    ret = switch_barrier_reg_write(device, config->group_id,
                                   SWITCH_BARRIER_REG_CONTROL, ctrl_val);

unlock_out:
    OPAL_THREAD_UNLOCK(&device->lock);
    return ret;
}

int switch_barrier_allocate_group(switch_barrier_device_t *device,
                                  uint32_t *group_id)
{
    int i;

    if (NULL == device || NULL == group_id) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&device->lock);

    for (i = 0; i < device->num_groups; i++) {
        if (!(device->group_allocation_mask & (1ULL << i))) {
            device->group_allocation_mask |= (1ULL << i);
            *group_id = i;
            OPAL_THREAD_UNLOCK(&device->lock);
            return OMPI_SUCCESS;
        }
    }

    OPAL_THREAD_UNLOCK(&device->lock);
    return OMPI_ERR_OUT_OF_RESOURCE;
}

int switch_barrier_free_group(switch_barrier_device_t *device,
                              uint32_t group_id)
{
    if (NULL == device) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (group_id >= (uint32_t)device->num_groups) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&device->lock);

    switch_barrier_reg_write(device, group_id,
                             SWITCH_BARRIER_REG_CONTROL, 0);

    device->group_allocation_mask &= ~(1ULL << group_id);

    OPAL_THREAD_UNLOCK(&device->lock);

    return OMPI_SUCCESS;
}

int switch_barrier_send_arrival(switch_barrier_device_t *device,
                                uint32_t group_id,
                                uint32_t member_id,
                                uint32_t sequence)
{
    switch_barrier_remote_store_msg_t msg;
    volatile uint64_t *arrival_reg;
    uint64_t arrival_value;
    int ret;

    if (NULL == device) {
        return OMPI_ERR_BAD_PARAM;
    }

    memset(&msg, 0, sizeof(msg));
    msg.msg_type = SWITCH_BARRIER_MSG_ARRIVE;
    msg.group_id = group_id;
    msg.member_id = member_id;
    msg.sequence = sequence;

    arrival_value = ((uint64_t)member_id << 32) | sequence;

    arrival_reg = (volatile uint64_t *)((char *)device->base_addr +
                   switch_barrier_calc_reg_addr(group_id,
                   SWITCH_BARRIER_REG_ARRIVAL_ADDR));

    *arrival_reg = arrival_value;
    opal_atomic_wmb();

    ret = OMPI_SUCCESS;

    return ret;
}

int switch_barrier_init_local_state(switch_barrier_local_state_t *local_state,
                                    switch_barrier_iommu_context_t *iommu_ctx)
{
    int ret;
    size_t page_size;

    if (NULL == local_state) {
        return OMPI_ERR_BAD_PARAM;
    }

    memset(local_state, 0, sizeof(*local_state));

    page_size = sysconf(_SC_PAGESIZE);
    local_state->flag_memory_size = page_size;

    local_state->flag_memory = mmap(NULL, local_state->flag_memory_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (local_state->flag_memory == MAP_FAILED) {
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    memset(local_state->flag_memory, 0, local_state->flag_memory_size);

    local_state->arrival_flag = (volatile uint64_t *)local_state->flag_memory;
    local_state->release_flag = (volatile uint64_t *)((char *)local_state->flag_memory + 64);
    local_state->expected_sequence = 1;

    if (NULL != iommu_ctx) {
        ret = switch_barrier_iommu_map(iommu_ctx, local_state->flag_memory,
                                        local_state->flag_memory_size,
                                        &local_state->iommu_iova);
        if (OMPI_SUCCESS != ret) {
            munmap(local_state->flag_memory, local_state->flag_memory_size);
            local_state->flag_memory = NULL;
            return ret;
        }
    }

    return OMPI_SUCCESS;
}

int switch_barrier_fini_local_state(switch_barrier_local_state_t *local_state,
                                    switch_barrier_iommu_context_t *iommu_ctx)
{
    if (NULL == local_state) {
        return OMPI_ERR_BAD_PARAM;
    }

    if (NULL != iommu_ctx && local_state->iommu_iova != 0) {
        switch_barrier_iommu_unmap(iommu_ctx, local_state->iommu_iova);
        local_state->iommu_iova = 0;
    }

    if (local_state->flag_memory != NULL && local_state->flag_memory != MAP_FAILED) {
        munmap(local_state->flag_memory, local_state->flag_memory_size);
        local_state->flag_memory = NULL;
    }

    local_state->arrival_flag = NULL;
    local_state->release_flag = NULL;

    return OMPI_SUCCESS;
}
