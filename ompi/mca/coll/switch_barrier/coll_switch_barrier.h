/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2024      Switch Barrier Accelerator Implementation
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_COLL_SWITCH_BARRIER_EXPORT_H
#define MCA_COLL_SWITCH_BARRIER_EXPORT_H

#include "ompi_config.h"

#include "mpi.h"

#include "opal/class/opal_object.h"
#include "ompi/mca/mca.h"

#include "ompi/constants.h"
#include "ompi/mca/coll/coll.h"
#include "ompi/mca/coll/base/base.h"
#include "ompi/communicator/communicator.h"

BEGIN_C_DECLS

/*
 * ============================================================================
 * Switch Barrier Accelerator Hardware Register Definitions
 * ============================================================================
 *
 * The switch barrier accelerator is implemented in the network switch hardware.
 * It provides hardware-accelerated barrier synchronization by:
 * 1. Receiving remote stores from all members when they reach the barrier
 * 2. Aggregating barrier arrival signals from all group members
 * 3. Broadcasting barrier release signals to all members via remote store
 * 4. Members poll local flag to detect barrier completion
 *
 * Register Map (per barrier group):
 * - NETWORK_ADDR_REG:   Network address of the switch accelerator
 * - GROUP_ID_REG:       Barrier group identifier (communication domain)
 * - MEMBER_MASK_REG:    Bitmask of members in this barrier group
 * - MEMBER_COUNT_REG:   Number of members in the barrier group
 * - CONTROL_REG:        Control register for barrier operations
 * - STATUS_REG:         Status register for barrier state
 * - ARRIVED_MASK_REG:   Bitmask of members that have arrived
 */

/* Switch barrier accelerator register offsets */
#define SWITCH_BARRIER_REG_NETWORK_ADDR      0x0000  /* Network address register */
#define SWITCH_BARRIER_REG_GROUP_ID          0x0008  /* Group ID register */
#define SWITCH_BARRIER_REG_MEMBER_MASK_LO    0x0010  /* Member mask (low 64 bits) */
#define SWITCH_BARRIER_REG_MEMBER_MASK_HI    0x0018  /* Member mask (high 64 bits) */
#define SWITCH_BARRIER_REG_MEMBER_COUNT      0x0020  /* Member count register */
#define SWITCH_BARRIER_REG_CONTROL           0x0028  /* Control register */
#define SWITCH_BARRIER_REG_STATUS            0x0030  /* Status register */
#define SWITCH_BARRIER_REG_ARRIVED_MASK_LO   0x0038  /* Arrived mask (low 64 bits) */
#define SWITCH_BARRIER_REG_ARRIVED_MASK_HI   0x0040  /* Arrived mask (high 64 bits) */
#define SWITCH_BARRIER_REG_LOCAL_MEMBER_ID   0x0048  /* Local member ID in group */
#define SWITCH_BARRIER_REG_RELEASE_ADDR      0x0050  /* Address for release signal */
#define SWITCH_BARRIER_REG_ARRIVAL_ADDR      0x0058  /* Address for arrival signal */

/* Control register bit definitions */
#define SWITCH_BARRIER_CTRL_ENABLE           (1ULL << 0)   /* Enable barrier group */
#define SWITCH_BARRIER_CTRL_RESET            (1ULL << 1)   /* Reset barrier state */
#define SWITCH_BARRIER_CTRL_ARM              (1ULL << 2)   /* Arm for next barrier */
#define SWITCH_BARRIER_CTRL_INTERRUPT_EN     (1ULL << 3)   /* Enable interrupt on complete */

/* Status register bit definitions */
#define SWITCH_BARRIER_STATUS_READY          (1ULL << 0)   /* Barrier group is ready */
#define SWITCH_BARRIER_STATUS_ACTIVE         (1ULL << 1)   /* Barrier is in progress */
#define SWITCH_BARRIER_STATUS_COMPLETE       (1ULL << 2)   /* All members arrived */
#define SWITCH_BARRIER_STATUS_ERROR          (1ULL << 3)   /* Error condition */

/* Maximum number of barrier groups supported per switch */
#define SWITCH_BARRIER_MAX_GROUPS            256

/* Maximum number of members per barrier group */
#define SWITCH_BARRIER_MAX_MEMBERS           128

/* Remote store message types */
#define SWITCH_BARRIER_MSG_ARRIVE            0x01  /* Member arrival notification */
#define SWITCH_BARRIER_MSG_RELEASE           0x02  /* Barrier release signal */

/*
 * ============================================================================
 * Switch Device and Control Plane Structures
 * ============================================================================
 */

/**
 * Switch barrier accelerator device handle
 */
typedef struct switch_barrier_device_t {
    void *base_addr;                     /* Base address for MMIO access */
    void *control_plane_handle;          /* Control plane connection handle */
    uint64_t network_addr;               /* Network address of this switch */
    int device_fd;                       /* Device file descriptor */
    int num_groups;                      /* Number of available barrier groups */
    uint64_t group_allocation_mask;      /* Bitmask of allocated groups */
    opal_mutex_t lock;                   /* Device access lock */
} switch_barrier_device_t;

/**
 * Barrier group configuration
 */
typedef struct switch_barrier_group_config_t {
    uint32_t group_id;                   /* Barrier group identifier */
    uint32_t member_count;               /* Number of members */
    uint64_t member_mask[2];             /* 128-bit member mask */
    uint64_t network_addrs[SWITCH_BARRIER_MAX_MEMBERS]; /* Member network addresses */
    uint32_t local_member_id;            /* This node's member ID in the group */
} switch_barrier_group_config_t;

/**
 * Local barrier state for flag polling
 */
typedef struct switch_barrier_local_state_t {
    volatile uint64_t *arrival_flag;     /* Local flag for arrival signal */
    volatile uint64_t *release_flag;     /* Local flag for release signal */
    uint64_t expected_sequence;          /* Expected sequence number for release */
    void *flag_memory;                   /* Allocated memory for flags */
    size_t flag_memory_size;             /* Size of flag memory */
    uint64_t iommu_iova;                 /* IOVA for IOMMU mapping */
} switch_barrier_local_state_t;

/*
 * ============================================================================
 * IOMMU Configuration Placeholder
 * ============================================================================
 */

/**
 * IOMMU mapping entry for DMA access from switch
 */
typedef struct switch_barrier_iommu_mapping_t {
    void *vaddr;                         /* Virtual address */
    uint64_t iova;                       /* I/O Virtual Address for device */
    size_t size;                         /* Mapping size */
    int prot;                            /* Protection flags */
    int valid;                           /* Mapping is valid */
} switch_barrier_iommu_mapping_t;

/**
 * IOMMU context for switch barrier
 */
typedef struct switch_barrier_iommu_context_t {
    int iommu_fd;                        /* IOMMU device file descriptor */
    int domain_id;                       /* IOMMU domain identifier */
    void *iommu_handle;                  /* Platform-specific IOMMU handle */
    switch_barrier_iommu_mapping_t *mappings; /* Array of mappings */
    int num_mappings;                    /* Number of active mappings */
    int max_mappings;                    /* Maximum mappings capacity */
    opal_mutex_t lock;                   /* IOMMU context lock */
} switch_barrier_iommu_context_t;

/*
 * ============================================================================
 * Control Plane API Functions
 * ============================================================================
 */

/**
 * Initialize connection to switch control plane
 *
 * @param[out] device   Device handle to initialize
 * @param[in]  dev_path Path to switch device (e.g., "/dev/switch_barrier0")
 *
 * @return OMPI_SUCCESS on success, error code otherwise
 */
int switch_barrier_control_plane_init(switch_barrier_device_t *device,
                                       const char *dev_path);

/**
 * Finalize connection to switch control plane
 *
 * @param[in] device Device handle to finalize
 *
 * @return OMPI_SUCCESS on success
 */
int switch_barrier_control_plane_fini(switch_barrier_device_t *device);

/**
 * Read a register from the switch barrier accelerator
 *
 * @param[in]  device   Device handle
 * @param[in]  group_id Barrier group ID
 * @param[in]  reg_offset Register offset
 * @param[out] value    Value read from register
 *
 * @return OMPI_SUCCESS on success, error code otherwise
 */
int switch_barrier_reg_read(switch_barrier_device_t *device,
                            uint32_t group_id,
                            uint32_t reg_offset,
                            uint64_t *value);

/**
 * Write a register to the switch barrier accelerator
 *
 * @param[in] device    Device handle
 * @param[in] group_id  Barrier group ID
 * @param[in] reg_offset Register offset
 * @param[in] value     Value to write
 *
 * @return OMPI_SUCCESS on success, error code otherwise
 */
int switch_barrier_reg_write(switch_barrier_device_t *device,
                             uint32_t group_id,
                             uint32_t reg_offset,
                             uint64_t value);

/**
 * Configure a barrier group on the switch
 *
 * @param[in] device Device handle
 * @param[in] config Group configuration
 *
 * @return OMPI_SUCCESS on success, error code otherwise
 */
int switch_barrier_configure_group(switch_barrier_device_t *device,
                                   switch_barrier_group_config_t *config);

/**
 * Allocate a barrier group ID
 *
 * @param[in]  device   Device handle
 * @param[out] group_id Allocated group ID
 *
 * @return OMPI_SUCCESS on success, OMPI_ERR_OUT_OF_RESOURCE if no groups available
 */
int switch_barrier_allocate_group(switch_barrier_device_t *device,
                                  uint32_t *group_id);

/**
 * Free a barrier group ID
 *
 * @param[in] device   Device handle
 * @param[in] group_id Group ID to free
 *
 * @return OMPI_SUCCESS on success
 */
int switch_barrier_free_group(switch_barrier_device_t *device,
                              uint32_t group_id);

/*
 * ============================================================================
 * IOMMU Configuration API Functions
 * ============================================================================
 */

/**
 * Initialize IOMMU context for switch barrier DMA access
 *
 * @param[out] ctx      IOMMU context to initialize
 * @param[in]  device   Switch device handle
 *
 * @return OMPI_SUCCESS on success, error code otherwise
 */
int switch_barrier_iommu_init(switch_barrier_iommu_context_t *ctx,
                               switch_barrier_device_t *device);

/**
 * Finalize IOMMU context
 *
 * @param[in] ctx IOMMU context to finalize
 *
 * @return OMPI_SUCCESS on success
 */
int switch_barrier_iommu_fini(switch_barrier_iommu_context_t *ctx);

/**
 * Map memory for DMA access from switch
 *
 * @param[in]  ctx   IOMMU context
 * @param[in]  vaddr Virtual address to map
 * @param[in]  size  Size of mapping
 * @param[out] iova  Resulting I/O virtual address
 *
 * @return OMPI_SUCCESS on success, error code otherwise
 */
int switch_barrier_iommu_map(switch_barrier_iommu_context_t *ctx,
                              void *vaddr,
                              size_t size,
                              uint64_t *iova);

/**
 * Unmap memory from DMA access
 *
 * @param[in] ctx  IOMMU context
 * @param[in] iova I/O virtual address to unmap
 *
 * @return OMPI_SUCCESS on success
 */
int switch_barrier_iommu_unmap(switch_barrier_iommu_context_t *ctx,
                                uint64_t iova);

/*
 * ============================================================================
 * Remote Store Mechanism (Non-RDMA)
 * ============================================================================
 */

/**
 * Remote store message structure for barrier signaling
 */
typedef struct switch_barrier_remote_store_msg_t {
    uint8_t  msg_type;                   /* SWITCH_BARRIER_MSG_ARRIVE or _RELEASE */
    uint8_t  reserved[3];                /* Reserved for alignment */
    uint32_t group_id;                   /* Barrier group ID */
    uint32_t member_id;                  /* Sender's member ID */
    uint32_t sequence;                   /* Barrier sequence number */
    uint64_t timestamp;                  /* Timestamp for debugging */
    uint64_t target_addr;                /* Target address for store */
    uint64_t store_value;                /* Value to store */
} switch_barrier_remote_store_msg_t;

/**
 * Send arrival notification to switch
 *
 * @param[in] device    Device handle
 * @param[in] group_id  Barrier group ID
 * @param[in] member_id Local member ID
 * @param[in] sequence  Barrier sequence number
 *
 * @return OMPI_SUCCESS on success, error code otherwise
 */
int switch_barrier_send_arrival(switch_barrier_device_t *device,
                                uint32_t group_id,
                                uint32_t member_id,
                                uint32_t sequence);

/**
 * Poll local flag for barrier release
 *
 * @param[in] local_state Local barrier state
 * @param[in] sequence    Expected sequence number
 *
 * @return true if barrier is released, false otherwise
 */
static inline bool switch_barrier_poll_release(
    switch_barrier_local_state_t *local_state,
    uint64_t sequence)
{
    /* Memory barrier to ensure we see the latest value */
    opal_atomic_rmb();
    return (*(local_state->release_flag) >= sequence);
}

/**
 * Initialize local barrier state with flag memory
 *
 * @param[out] local_state Local state to initialize
 * @param[in]  iommu_ctx   IOMMU context for mapping
 *
 * @return OMPI_SUCCESS on success, error code otherwise
 */
int switch_barrier_init_local_state(switch_barrier_local_state_t *local_state,
                                    switch_barrier_iommu_context_t *iommu_ctx);

/**
 * Finalize local barrier state
 *
 * @param[in] local_state Local state to finalize
 * @param[in] iommu_ctx   IOMMU context
 *
 * @return OMPI_SUCCESS on success
 */
int switch_barrier_fini_local_state(switch_barrier_local_state_t *local_state,
                                    switch_barrier_iommu_context_t *iommu_ctx);

/*
 * ============================================================================
 * MCA Collective Component Interface
 * ============================================================================
 */

/* API functions */
int mca_coll_switch_barrier_init_query(bool enable_progress_threads,
                                        bool enable_mpi_threads);

mca_coll_base_module_t *mca_coll_switch_barrier_comm_query(
    struct ompi_communicator_t *comm,
    int *priority);

int mca_coll_switch_barrier_barrier(struct ompi_communicator_t *comm,
                                    mca_coll_base_module_t *module);

int mca_coll_switch_barrier_ibarrier(struct ompi_communicator_t *comm,
                                     ompi_request_t **request,
                                     mca_coll_base_module_t *module);

/*
 * ============================================================================
 * Module and Component Types
 * ============================================================================
 */

/**
 * Per-communicator module data for switch barrier
 */
typedef struct mca_coll_switch_barrier_module_t {
    mca_coll_base_module_t super;

    /* Fallback collective functions */
    mca_coll_base_comm_coll_t c_coll;

    /* Switch barrier specific data */
    switch_barrier_device_t *device;             /* Switch device handle */
    switch_barrier_group_config_t group_config;  /* Group configuration */
    switch_barrier_local_state_t local_state;    /* Local barrier state */
    switch_barrier_iommu_context_t *iommu_ctx;   /* IOMMU context (shared) */
    
    uint32_t barrier_sequence;                   /* Current barrier sequence */
    bool offload_enabled;                        /* Is offload enabled for this comm */
} mca_coll_switch_barrier_module_t;

OBJ_CLASS_DECLARATION(mca_coll_switch_barrier_module_t);

/**
 * Component data for switch barrier
 */
typedef struct mca_coll_switch_barrier_component_t {
    mca_coll_base_component_3_0_0_t super;

    int priority;                                /* Component priority */
    int disable_switch_barrier;                  /* Force disable */
    char *device_path;                           /* Path to switch device */
    int min_comm_size;                           /* Minimum comm size for offload */
    
    /* Global switch device and IOMMU context */
    switch_barrier_device_t device;
    switch_barrier_iommu_context_t iommu_ctx;
    bool initialized;
} mca_coll_switch_barrier_component_t;

/* Globally exported component */
OMPI_DECLSPEC extern mca_coll_switch_barrier_component_t mca_coll_switch_barrier_component;

END_C_DECLS

#endif /* MCA_COLL_SWITCH_BARRIER_EXPORT_H */
