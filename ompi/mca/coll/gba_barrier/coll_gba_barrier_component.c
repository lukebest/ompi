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
 * GBA Barrier MCA Component Registration
 */

#include "ompi_config.h"

#include <string.h>

#include "mpi.h"
#include "ompi/constants.h"
#include "opal/util/output.h"

#include "coll_gba_barrier.h"

static int gba_component_register(void);
static int gba_component_open(void);
static int gba_component_close(void);

/*
 * Global component instance
 */
mca_coll_gba_component_t mca_coll_gba_component = {
    .super = {
        .collm_version = {
            MCA_COLL_BASE_VERSION_3_0_0,

            .mca_component_name = "gba_barrier",
            MCA_BASE_MAKE_VERSION(component, OMPI_MAJOR_VERSION, OMPI_MINOR_VERSION,
                                  OMPI_RELEASE_VERSION),

            .mca_open_component = gba_component_open,
            .mca_close_component = gba_component_close,
            .mca_register_component_params = gba_component_register,
        },
        .collm_data = {
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        },

        .collm_init_query = mca_coll_gba_init_query,
        .collm_comm_query = mca_coll_gba_comm_query,
    },

    /* Default configuration */
    .priority = 100,
    .disable = 0,
    .device_path = "/dev/gba0",
    .min_comm_size = 2,
    .initialized = false,
};

MCA_BASE_COMPONENT_INIT(ompi, coll, gba_barrier)

static int gba_component_register(void)
{
    /* Priority: higher than basic/tuned to prefer hardware acceleration */
    mca_coll_gba_component.priority = 100;
    (void) mca_base_component_var_register(
        &mca_coll_gba_component.super.collm_version,
        "priority",
        "Priority of GBA barrier component (default: 100)",
        MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
        OPAL_INFO_LVL_6,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_coll_gba_component.priority);

    /* Disable flag */
    mca_coll_gba_component.disable = 0;
    (void) mca_base_component_var_register(
        &mca_coll_gba_component.super.collm_version,
        "disable",
        "Disable GBA hardware offload (0=enabled, 1=disabled)",
        MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
        OPAL_INFO_LVL_2,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_coll_gba_component.disable);

    /* Device path */
    mca_coll_gba_component.device_path = "/dev/gba0";
    (void) mca_base_component_var_register(
        &mca_coll_gba_component.super.collm_version,
        "device_path",
        "Path to GBA device (default: /dev/gba0)",
        MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
        OPAL_INFO_LVL_4,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_coll_gba_component.device_path);

    /* Minimum communicator size */
    mca_coll_gba_component.min_comm_size = 2;
    (void) mca_base_component_var_register(
        &mca_coll_gba_component.super.collm_version,
        "min_comm_size",
        "Minimum communicator size for GBA offload (default: 2)",
        MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
        OPAL_INFO_LVL_6,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_coll_gba_component.min_comm_size);

    return OMPI_SUCCESS;
}

static int gba_component_open(void)
{
    return OMPI_SUCCESS;
}

static int gba_component_close(void)
{
    if (mca_coll_gba_component.initialized) {
        gba_device_fini(&mca_coll_gba_component.device);
        gba_dma_fini(&mca_coll_gba_component.dma_ctx);
        mca_coll_gba_component.initialized = false;
    }
    return OMPI_SUCCESS;
}

int mca_coll_gba_init_query(bool enable_progress_threads,
                            bool enable_mpi_threads)
{
    int ret;

    if (mca_coll_gba_component.disable) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: disabled by user");
        return OMPI_ERR_NOT_AVAILABLE;
    }

    if (mca_coll_gba_component.initialized) {
        return OMPI_SUCCESS;
    }

    /* Initialize GBA device */
    ret = gba_device_init(&mca_coll_gba_component.device,
                          mca_coll_gba_component.device_path);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: failed to init device %s",
                            mca_coll_gba_component.device_path);
        return ret;
    }

    /* Initialize DMA context */
    ret = gba_dma_init(&mca_coll_gba_component.dma_ctx,
                       &mca_coll_gba_component.device);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: failed to init DMA context");
        gba_device_fini(&mca_coll_gba_component.device);
        return ret;
    }

    mca_coll_gba_component.initialized = true;

    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier: component initialized successfully");

    return OMPI_SUCCESS;
}
