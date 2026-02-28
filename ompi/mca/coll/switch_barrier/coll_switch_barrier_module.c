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

#include <string.h>
#include <stdio.h>

#include "coll_switch_barrier.h"

#include "mpi.h"

#include "opal/util/show_help.h"

#include "ompi/constants.h"
#include "ompi/communicator/communicator.h"
#include "ompi/mca/coll/coll.h"
#include "ompi/mca/coll/base/base.h"
#include "ompi/proc/proc.h"

static int mca_coll_switch_barrier_module_enable(mca_coll_base_module_t *module,
                                                  struct ompi_communicator_t *comm);
static int mca_coll_switch_barrier_module_disable(mca_coll_base_module_t *module,
                                                   struct ompi_communicator_t *comm);

static void mca_coll_switch_barrier_module_construct(mca_coll_switch_barrier_module_t *module)
{
    memset(&(module->c_coll), 0, sizeof(module->c_coll));
    module->device = NULL;
    memset(&module->group_config, 0, sizeof(module->group_config));
    memset(&module->local_state, 0, sizeof(module->local_state));
    module->iommu_ctx = NULL;
    module->barrier_sequence = 0;
    module->offload_enabled = false;
}

static void mca_coll_switch_barrier_module_destruct(mca_coll_switch_barrier_module_t *module)
{
    if (module->offload_enabled && module->device != NULL) {
        switch_barrier_free_group(module->device, module->group_config.group_id);
        switch_barrier_fini_local_state(&module->local_state, module->iommu_ctx);
    }
}

OBJ_CLASS_INSTANCE(mca_coll_switch_barrier_module_t, mca_coll_base_module_t,
                   mca_coll_switch_barrier_module_construct,
                   mca_coll_switch_barrier_module_destruct);

static int switch_barrier_configure_comm_domain(
    mca_coll_switch_barrier_module_t *module,
    struct ompi_communicator_t *comm)
{
    int ret;
    int comm_size;
    int my_rank;
    int i;
    ompi_proc_t *proc;
    uint32_t group_id;

    comm_size = ompi_comm_size(comm);
    my_rank = ompi_comm_rank(comm);

    if (comm_size > SWITCH_BARRIER_MAX_MEMBERS) {
        return OMPI_ERR_NOT_SUPPORTED;
    }

    ret = switch_barrier_allocate_group(module->device, &group_id);
    if (OMPI_SUCCESS != ret) {
        return ret;
    }

    module->group_config.group_id = group_id;
    module->group_config.member_count = comm_size;
    module->group_config.local_member_id = my_rank;

    module->group_config.member_mask[0] = 0;
    module->group_config.member_mask[1] = 0;
    for (i = 0; i < comm_size; i++) {
        if (i < 64) {
            module->group_config.member_mask[0] |= (1ULL << i);
        } else {
            module->group_config.member_mask[1] |= (1ULL << (i - 64));
        }
    }

    for (i = 0; i < comm_size; i++) {
        proc = ompi_comm_peer_lookup(comm, i);
        if (NULL == proc) {
            switch_barrier_free_group(module->device, group_id);
            return OMPI_ERR_NOT_FOUND;
        }
        module->group_config.network_addrs[i] = (uint64_t)(uintptr_t)proc;
    }

    ret = switch_barrier_init_local_state(&module->local_state, module->iommu_ctx);
    if (OMPI_SUCCESS != ret) {
        switch_barrier_free_group(module->device, group_id);
        return ret;
    }

    ret = switch_barrier_configure_group(module->device, &module->group_config);
    if (OMPI_SUCCESS != ret) {
        switch_barrier_fini_local_state(&module->local_state, module->iommu_ctx);
        switch_barrier_free_group(module->device, group_id);
        return ret;
    }

    module->barrier_sequence = 0;
    module->offload_enabled = true;

    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:switch_barrier: configured comm domain for comm %p "
                        "(size=%d, group_id=%u, local_id=%u)",
                        (void *)comm, comm_size, group_id, (unsigned)my_rank);

    return OMPI_SUCCESS;
}

mca_coll_base_module_t *mca_coll_switch_barrier_comm_query(
    struct ompi_communicator_t *comm,
    int *priority)
{
    mca_coll_switch_barrier_module_t *module;
    int comm_size;

    if (!mca_coll_switch_barrier_component.initialized) {
        return NULL;
    }

    if (OMPI_COMM_IS_INTER(comm)) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:switch_barrier: inter-communicators not supported");
        return NULL;
    }

    comm_size = ompi_comm_size(comm);
    if (comm_size < mca_coll_switch_barrier_component.min_comm_size) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:switch_barrier: comm size %d below minimum %d",
                            comm_size, mca_coll_switch_barrier_component.min_comm_size);
        return NULL;
    }

    if (comm_size > SWITCH_BARRIER_MAX_MEMBERS) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:switch_barrier: comm size %d exceeds max %d",
                            comm_size, SWITCH_BARRIER_MAX_MEMBERS);
        return NULL;
    }

    module = OBJ_NEW(mca_coll_switch_barrier_module_t);
    if (NULL == module) {
        return NULL;
    }

    module->device = &mca_coll_switch_barrier_component.device;
    module->iommu_ctx = &mca_coll_switch_barrier_component.iommu_ctx;

    *priority = mca_coll_switch_barrier_component.priority;

    module->super.coll_module_enable = mca_coll_switch_barrier_module_enable;
    module->super.coll_module_disable = mca_coll_switch_barrier_module_disable;
    module->super.coll_barrier = mca_coll_switch_barrier_barrier;
    module->super.coll_ibarrier = mca_coll_switch_barrier_ibarrier;

    return &(module->super);
}

#define SWITCH_BARRIER_INSTALL_COLL_API(__comm, __module, __api) \
    do { \
        if ((__comm)->c_coll->coll_##__api) { \
            MCA_COLL_SAVE_API(__comm, __api, \
                              (__module)->c_coll.coll_##__api, \
                              (__module)->c_coll.coll_##__api##_module, \
                              "switch_barrier"); \
            MCA_COLL_INSTALL_API(__comm, __api, \
                                 mca_coll_switch_barrier_##__api, \
                                 &__module->super, "switch_barrier"); \
        } \
    } while (0)

#define SWITCH_BARRIER_UNINSTALL_COLL_API(__comm, __module, __api) \
    do { \
        if (&(__module)->super == (__comm)->c_coll->coll_##__api##_module) { \
            MCA_COLL_INSTALL_API(__comm, __api, \
                                 (__module)->c_coll.coll_##__api, \
                                 (__module)->c_coll.coll_##__api##_module, \
                                 "switch_barrier"); \
            (__module)->c_coll.coll_##__api##_module = NULL; \
            (__module)->c_coll.coll_##__api = NULL; \
        } \
    } while (0)

static int mca_coll_switch_barrier_module_enable(mca_coll_base_module_t *module,
                                                  struct ompi_communicator_t *comm)
{
    mca_coll_switch_barrier_module_t *s = (mca_coll_switch_barrier_module_t *)module;
    int ret;

    ret = switch_barrier_configure_comm_domain(s, comm);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:switch_barrier: failed to configure comm domain, "
                            "using fallback barrier");
        return ret;
    }

    SWITCH_BARRIER_INSTALL_COLL_API(comm, s, barrier);
    SWITCH_BARRIER_INSTALL_COLL_API(comm, s, ibarrier);

    return OMPI_SUCCESS;
}

static int mca_coll_switch_barrier_module_disable(mca_coll_base_module_t *module,
                                                   struct ompi_communicator_t *comm)
{
    mca_coll_switch_barrier_module_t *s = (mca_coll_switch_barrier_module_t *)module;

    SWITCH_BARRIER_UNINSTALL_COLL_API(comm, s, barrier);
    SWITCH_BARRIER_UNINSTALL_COLL_API(comm, s, ibarrier);

    return OMPI_SUCCESS;
}

int mca_coll_switch_barrier_barrier(struct ompi_communicator_t *comm,
                                    mca_coll_base_module_t *module)
{
    mca_coll_switch_barrier_module_t *s = (mca_coll_switch_barrier_module_t *)module;
    uint32_t sequence;
    int ret;

    if (!s->offload_enabled) {
        return s->c_coll.coll_barrier(comm, s->c_coll.coll_barrier_module);
    }

    sequence = ++s->barrier_sequence;

    ret = switch_barrier_send_arrival(s->device,
                                       s->group_config.group_id,
                                       s->group_config.local_member_id,
                                       sequence);
    if (OMPI_SUCCESS != ret) {
        return s->c_coll.coll_barrier(comm, s->c_coll.coll_barrier_module);
    }

    while (!switch_barrier_poll_release(&s->local_state, sequence)) {
        opal_progress();
    }

    return MPI_SUCCESS;
}

int mca_coll_switch_barrier_ibarrier(struct ompi_communicator_t *comm,
                                     ompi_request_t **request,
                                     mca_coll_base_module_t *module)
{
    mca_coll_switch_barrier_module_t *s = (mca_coll_switch_barrier_module_t *)module;

    if (!s->offload_enabled) {
        return s->c_coll.coll_ibarrier(comm, request, s->c_coll.coll_ibarrier_module);
    }

    return s->c_coll.coll_ibarrier(comm, request, s->c_coll.coll_ibarrier_module);
}
