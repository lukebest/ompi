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

#include "mpi.h"
#include "ompi/constants.h"
#include "coll_switch_barrier.h"

static int switch_barrier_register(void);

const char *mca_coll_switch_barrier_component_version_string =
    "Open MPI switch barrier collective MCA component version " OMPI_VERSION;

mca_coll_switch_barrier_component_t mca_coll_switch_barrier_component = {
    {
        .collm_version = {
            MCA_COLL_BASE_VERSION_3_0_0,

            .mca_component_name = "switch_barrier",
            MCA_BASE_MAKE_VERSION(component, OMPI_MAJOR_VERSION, OMPI_MINOR_VERSION,
                                  OMPI_RELEASE_VERSION),

            .mca_register_component_params = switch_barrier_register,
        },
        .collm_data = {
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        },

        .collm_init_query = mca_coll_switch_barrier_init_query,
        .collm_comm_query = mca_coll_switch_barrier_comm_query,
    },

    .priority = 90,
    .disable_switch_barrier = 0,
    .device_path = NULL,
    .min_comm_size = 2,
    .initialized = false,
};

MCA_BASE_COMPONENT_INIT(ompi, coll, switch_barrier)

static int switch_barrier_register(void)
{
    mca_coll_switch_barrier_component.priority = 90;
    (void) mca_base_component_var_register(
        &mca_coll_switch_barrier_component.super.collm_version,
        "priority",
        "Priority of the switch barrier coll component",
        MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
        OPAL_INFO_LVL_6,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_coll_switch_barrier_component.priority);

    mca_coll_switch_barrier_component.disable_switch_barrier = 0;
    (void) mca_base_component_var_register(
        &mca_coll_switch_barrier_component.super.collm_version,
        "disable",
        "Disable switch barrier accelerator offload",
        MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
        OPAL_INFO_LVL_2,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_coll_switch_barrier_component.disable_switch_barrier);

    mca_coll_switch_barrier_component.device_path = "/dev/switch_barrier0";
    (void) mca_base_component_var_register(
        &mca_coll_switch_barrier_component.super.collm_version,
        "device_path",
        "Path to switch barrier accelerator device",
        MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
        OPAL_INFO_LVL_4,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_coll_switch_barrier_component.device_path);

    mca_coll_switch_barrier_component.min_comm_size = 2;
    (void) mca_base_component_var_register(
        &mca_coll_switch_barrier_component.super.collm_version,
        "min_comm_size",
        "Minimum communicator size for switch barrier offload",
        MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
        OPAL_INFO_LVL_6,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_coll_switch_barrier_component.min_comm_size);

    return OMPI_SUCCESS;
}

int mca_coll_switch_barrier_init_query(bool enable_progress_threads,
                                        bool enable_mpi_threads)
{
    int ret;

    if (mca_coll_switch_barrier_component.disable_switch_barrier) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:switch_barrier: disabled by user");
        return OMPI_ERR_NOT_AVAILABLE;
    }

    if (mca_coll_switch_barrier_component.initialized) {
        return OMPI_SUCCESS;
    }

    ret = switch_barrier_control_plane_init(
        &mca_coll_switch_barrier_component.device,
        mca_coll_switch_barrier_component.device_path);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:switch_barrier: failed to init control plane");
        return ret;
    }

    ret = switch_barrier_iommu_init(
        &mca_coll_switch_barrier_component.iommu_ctx,
        &mca_coll_switch_barrier_component.device);
    if (OMPI_SUCCESS != ret) {
        switch_barrier_control_plane_fini(&mca_coll_switch_barrier_component.device);
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:switch_barrier: failed to init IOMMU context");
        return ret;
    }

    mca_coll_switch_barrier_component.initialized = true;

    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:switch_barrier: component initialized");

    return OMPI_SUCCESS;
}
