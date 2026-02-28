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

#define IOMMU_DEFAULT_MAX_MAPPINGS 64

int switch_barrier_iommu_init(switch_barrier_iommu_context_t *ctx,
                               switch_barrier_device_t *device)
{
    if (NULL == ctx) {
        return OMPI_ERR_BAD_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    OBJ_CONSTRUCT(&ctx->lock, opal_mutex_t);

    ctx->iommu_fd = -1;
    ctx->domain_id = -1;
    ctx->iommu_handle = NULL;

    ctx->max_mappings = IOMMU_DEFAULT_MAX_MAPPINGS;
    ctx->mappings = (switch_barrier_iommu_mapping_t *)calloc(
        ctx->max_mappings, sizeof(switch_barrier_iommu_mapping_t));
    if (NULL == ctx->mappings) {
        OBJ_DESTRUCT(&ctx->lock);
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    ctx->num_mappings = 0;

    /*
     * TODO: Platform-specific IOMMU initialization
     * 
     * Linux VFIO example:
     *   ctx->iommu_fd = open("/dev/vfio/vfio", O_RDWR);
     *   ioctl(ctx->iommu_fd, VFIO_GET_API_VERSION);
     *   ioctl(ctx->iommu_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
     *
     * Intel IOMMU (VT-d):
     *   Use DMAR tables and kernel IOMMU driver
     *
     * ARM SMMU:
     *   Configure stream IDs and context banks
     */

    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "switch_barrier: IOMMU context initialized (placeholder)");

    return OMPI_SUCCESS;
}

int switch_barrier_iommu_fini(switch_barrier_iommu_context_t *ctx)
{
    int i;

    if (NULL == ctx) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&ctx->lock);

    for (i = 0; i < ctx->num_mappings; i++) {
        if (ctx->mappings[i].valid) {
            /*
             * TODO: Platform-specific unmap
             * ioctl(ctx->iommu_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
             */
            ctx->mappings[i].valid = 0;
        }
    }

    if (ctx->mappings != NULL) {
        free(ctx->mappings);
        ctx->mappings = NULL;
    }

    if (ctx->iommu_fd >= 0) {
        close(ctx->iommu_fd);
        ctx->iommu_fd = -1;
    }

    OPAL_THREAD_UNLOCK(&ctx->lock);
    OBJ_DESTRUCT(&ctx->lock);

    return OMPI_SUCCESS;
}

int switch_barrier_iommu_map(switch_barrier_iommu_context_t *ctx,
                              void *vaddr,
                              size_t size,
                              uint64_t *iova)
{
    int i;
    int slot = -1;

    if (NULL == ctx || NULL == vaddr || NULL == iova || 0 == size) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&ctx->lock);

    for (i = 0; i < ctx->max_mappings; i++) {
        if (!ctx->mappings[i].valid) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        OPAL_THREAD_UNLOCK(&ctx->lock);
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    /*
     * TODO: Platform-specific DMA mapping
     *
     * Linux VFIO example:
     *   struct vfio_iommu_type1_dma_map dma_map = {
     *       .argsz = sizeof(dma_map),
     *       .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
     *       .vaddr = (uint64_t)vaddr,
     *       .iova = allocated_iova,
     *       .size = size,
     *   };
     *   ioctl(ctx->iommu_fd, VFIO_IOMMU_MAP_DMA, &dma_map);
     *
     * For now, use identity mapping (VA == IOVA) as placeholder
     */

    ctx->mappings[slot].vaddr = vaddr;
    ctx->mappings[slot].iova = (uint64_t)(uintptr_t)vaddr;
    ctx->mappings[slot].size = size;
    ctx->mappings[slot].prot = PROT_READ | PROT_WRITE;
    ctx->mappings[slot].valid = 1;

    *iova = ctx->mappings[slot].iova;

    if (slot >= ctx->num_mappings) {
        ctx->num_mappings = slot + 1;
    }

    OPAL_THREAD_UNLOCK(&ctx->lock);

    opal_output_verbose(20, ompi_coll_base_framework.framework_output,
                        "switch_barrier: IOMMU mapped vaddr=%p size=%zu iova=0x%lx",
                        vaddr, size, (unsigned long)*iova);

    return OMPI_SUCCESS;
}

int switch_barrier_iommu_unmap(switch_barrier_iommu_context_t *ctx,
                                uint64_t iova)
{
    int i;

    if (NULL == ctx) {
        return OMPI_ERR_BAD_PARAM;
    }

    OPAL_THREAD_LOCK(&ctx->lock);

    for (i = 0; i < ctx->num_mappings; i++) {
        if (ctx->mappings[i].valid && ctx->mappings[i].iova == iova) {
            /*
             * TODO: Platform-specific DMA unmap
             *
             * struct vfio_iommu_type1_dma_unmap unmap = {
             *     .argsz = sizeof(unmap),
             *     .iova = iova,
             *     .size = ctx->mappings[i].size,
             * };
             * ioctl(ctx->iommu_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
             */

            ctx->mappings[i].valid = 0;
            ctx->mappings[i].vaddr = NULL;
            ctx->mappings[i].iova = 0;
            ctx->mappings[i].size = 0;

            OPAL_THREAD_UNLOCK(&ctx->lock);

            opal_output_verbose(20, ompi_coll_base_framework.framework_output,
                                "switch_barrier: IOMMU unmapped iova=0x%lx",
                                (unsigned long)iova);

            return OMPI_SUCCESS;
        }
    }

    OPAL_THREAD_UNLOCK(&ctx->lock);
    return OMPI_ERR_NOT_FOUND;
}
