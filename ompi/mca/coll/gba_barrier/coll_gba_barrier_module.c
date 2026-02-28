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
 * GBA Barrier Module - Per-communicator implementation
 */

#include "ompi_config.h"

#include <string.h>
#include <stdio.h>

#include "coll_gba_barrier.h"

#include "mpi.h"
#include "opal/util/show_help.h"
#include "ompi/constants.h"
#include "ompi/communicator/communicator.h"
#include "ompi/mca/coll/coll.h"
#include "ompi/mca/coll/base/base.h"
#include "ompi/proc/proc.h"

static int mca_coll_gba_module_enable(mca_coll_base_module_t *module,
                                       struct ompi_communicator_t *comm);
static int mca_coll_gba_module_disable(mca_coll_base_module_t *module,
                                        struct ompi_communicator_t *comm);

static void mca_coll_gba_module_construct(mca_coll_gba_module_t *module)
{
    memset(&module->config, 0, sizeof(module->config));
    memset(&module->local_state, 0, sizeof(module->local_state));
    module->device = NULL;
    module->barrier_seq = 0;
    module->offload_enabled = false;
    module->previous_barrier = NULL;
    module->previous_barrier_module = NULL;
    module->previous_ibarrier = NULL;
    module->previous_ibarrier_module = NULL;
}

static void mca_coll_gba_module_destruct(mca_coll_gba_module_t *module)
{
    if (module->offload_enabled && module->device != NULL) {
        gba_free_group(module->device, module->config.group_id);
        gba_local_state_fini(&module->local_state,
                             &mca_coll_gba_component.dma_ctx);
        module->offload_enabled = false;
    }
}

OBJ_CLASS_INSTANCE(mca_coll_gba_module_t,
                   mca_coll_base_module_t,
                   mca_coll_gba_module_construct,
                   mca_coll_gba_module_destruct);

/**
 * Configure barrier group for a communicator
 */
static int gba_configure_comm_domain(mca_coll_gba_module_t *module,
                                      struct ompi_communicator_t *comm)
{
    int ret;
    int comm_size, my_rank;
    uint32_t group_id;
    int i;

    comm_size = ompi_comm_size(comm);
    my_rank = ompi_comm_rank(comm);

    /* Check hardware limits */
    if (comm_size > GBA_MAX_MEMBERS) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: comm size %d exceeds max %d",
                            comm_size, GBA_MAX_MEMBERS);
        return OMPI_ERR_NOT_SUPPORTED;
    }

    /* Allocate a barrier group */
    ret = gba_allocate_group(module->device, &group_id);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: no available groups");
        return ret;
    }

    /* Initialize group configuration */
    module->config.group_id = group_id;
    module->config.member_count = comm_size;
    module->config.local_member_id = my_rank;

    /* Build member mask (708 bits = 12 x 64-bit words) */
    memset(module->config.member_mask, 0, sizeof(module->config.member_mask));
    for (i = 0; i < comm_size; i++) {
        int word_idx = i / 64;
        int bit_idx = i % 64;
        module->config.member_mask[word_idx] |= (1ULL << bit_idx);
    }

    /* Initialize local state for release polling */
    ret = gba_local_state_init(&module->local_state,
                               &mca_coll_gba_component.dma_ctx);
    if (OMPI_SUCCESS != ret) {
        gba_free_group(module->device, group_id);
        return ret;
    }

    /* Configure the GBA group */
    ret = gba_configure_group(module->device, &module->config);
    if (OMPI_SUCCESS != ret) {
        gba_local_state_fini(&module->local_state,
                             &mca_coll_gba_component.dma_ctx);
        gba_free_group(module->device, group_id);
        return ret;
    }

    module->barrier_seq = 0;
    module->offload_enabled = true;

    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: configured group %u for comm %p "
                        "(size=%d, local_id=%u)",
                        group_id, (void *)comm, comm_size, my_rank);

    return OMPI_SUCCESS;
}

mca_coll_base_module_t *mca_coll_gba_comm_query(
    struct ompi_communicator_t *comm, int *priority)
{
    mca_coll_gba_module_t *module;
    int comm_size;

    if (!mca_coll_gba_component.initialized) {
        return NULL;
    }

    /* Inter-communicators not supported */
    if (OMPI_COMM_IS_INTER(comm)) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: inter-communicators not supported");
        return NULL;
    }

    comm_size = ompi_comm_size(comm);

    /* Check minimum size */
    if (comm_size < mca_coll_gba_component.min_comm_size) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: comm size %d below minimum %d",
                            comm_size, mca_coll_gba_component.min_comm_size);
        return NULL;
    }

    /* Check maximum size (708 ports) */
    if (comm_size > GBA_MAX_MEMBERS) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: comm size %d exceeds max %d",
                            comm_size, GBA_MAX_MEMBERS);
        return NULL;
    }

    /* Allocate module */
    module = OBJ_NEW(mca_coll_gba_module_t);
    if (NULL == module) {
        return NULL;
    }

    module->device = &mca_coll_gba_component.device;

    *priority = mca_coll_gba_component.priority;

    /* Set module function pointers */
    module->super.coll_module_enable = mca_coll_gba_module_enable;
    module->super.coll_module_disable = mca_coll_gba_module_disable;
    module->super.coll_barrier = mca_coll_gba_barrier;
    module->super.coll_ibarrier = mca_coll_gba_ibarrier;

    return &module->super;
}

static int mca_coll_gba_module_enable(mca_coll_base_module_t *module,
                                       struct ompi_communicator_t *comm)
{
    mca_coll_gba_module_t *m = (mca_coll_gba_module_t *)module;
    int ret;

    /* Save previous barrier functions for fallback */
    MCA_COLL_SAVE_API(comm, barrier, m->previous_barrier,
                       m->previous_barrier_module, "gba_barrier");
    MCA_COLL_SAVE_API(comm, ibarrier, m->previous_ibarrier,
                       m->previous_ibarrier_module, "gba_barrier");

    /* Configure GBA for this communicator */
    ret = gba_configure_comm_domain(m, comm);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: failed to configure, using fallback");
        /* Install our functions anyway; they will fallback to previous */
    }

    /* Install GBA barrier functions */
    MCA_COLL_INSTALL_API(comm, barrier, mca_coll_gba_barrier,
                          &m->super, "gba_barrier");
    MCA_COLL_INSTALL_API(comm, ibarrier, mca_coll_gba_ibarrier,
                          &m->super, "gba_barrier");

    return OMPI_SUCCESS;
}

static int mca_coll_gba_module_disable(mca_coll_base_module_t *module,
                                        struct ompi_communicator_t *comm)
{
    mca_coll_gba_module_t *m = (mca_coll_gba_module_t *)module;

    /* Restore previous functions */
    if (m->previous_barrier_module != NULL) {
        MCA_COLL_INSTALL_API(comm, barrier, m->previous_barrier,
                              m->previous_barrier_module, "gba_barrier");
    }
    if (m->previous_ibarrier_module != NULL) {
        MCA_COLL_INSTALL_API(comm, ibarrier, m->previous_ibarrier,
                              m->previous_ibarrier_module, "gba_barrier");
    }

    return OMPI_SUCCESS;
}

/**
 * Blocking barrier implementation using GBA hardware
 *
 * Protocol:
 * 1. Each rank sends arrival notification via remote store to GBA
 * 2. GBA hardware aggregates all arrivals
 * 3. When all members arrived, GBA broadcasts release via remote store
 * 4. Each rank polls local release flag for completion
 */
int mca_coll_gba_barrier(struct ompi_communicator_t *comm,
                          mca_coll_base_module_t *module)
{
    mca_coll_gba_module_t *m = (mca_coll_gba_module_t *)module;
    uint32_t sequence;
    int ret;

    /* Use fallback if GBA not enabled */
    if (!m->offload_enabled) {
        return m->previous_barrier(comm, m->previous_barrier_module);
    }

    /* Increment barrier sequence */
    sequence = ++m->barrier_seq;

    /*
     * Step 1: Send arrival notification to GBA via remote store
     *
     * This performs a remote store to the GBA's arrival register.
     * The store payload contains:
     *   - group_id: identifies which barrier group
     *   - member_id: identifies this rank within the group
     *   - sequence: current barrier sequence number
     */
    ret = gba_send_arrival(m->device,
                           m->config.group_id,
                           m->config.local_member_id,
                           sequence);
    if (OMPI_SUCCESS != ret) {
        /* Fallback on error */
        return m->previous_barrier(comm, m->previous_barrier_module);
    }

    /*
     * Step 2: Poll local release flag
     *
     * The GBA hardware will, after detecting all members have arrived:
     * 1. Atomically increment the release sequence
     * 2. For each member, perform a remote store to their release_flag
     *    with the new sequence value
     *
     * We poll until our local flag shows the expected sequence.
     */
    while (!gba_poll_release(&m->local_state, sequence)) {
        /* Allow other MPI progress to proceed */
        opal_progress();
    }

    return MPI_SUCCESS;
}

/**
 * Non-blocking barrier (currently uses fallback)
 *
 * TODO: Implement true non-blocking barrier with GBA hardware support.
 * This would require:
 * - Separate completion detection mechanism
 * - Request structure for tracking
 * - Progress function integration
 */
int mca_coll_gba_ibarrier(struct ompi_communicator_t *comm,
                           ompi_request_t **request,
                           mca_coll_base_module_t *module)
{
    mca_coll_gba_module_t *m = (mca_coll_gba_module_t *)module;

    /* For now, use fallback implementation */
    return m->previous_ibarrier(comm, request, m->previous_ibarrier_module);
}
