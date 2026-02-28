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
 *
 * Global Barrier Accelerator (GBA) for MPI Barrier
 *
 * The GBA is a hardware ASIC unit in network switches that provides
 * hardware-accelerated barrier synchronization through remote store semantics.
 *
 * Key Features:
 * - 32 concurrent barrier groups per switch
 * - Up to 708 members per group (corresponding to physical ports)
 * - Remote store semantic for arrival signaling
 * - Hardware aggregation of barrier arrivals
 * - Broadcast release via remote store to all members
 * - Local flag polling for completion detection
 */

#ifndef MCA_COLL_GBA_BARRIER_EXPORT_H
#define MCA_COLL_GBA_BARRIER_EXPORT_H

#include "ompi_config.h"

#include "mpi.h"

#include "opal/class/opal_object.h"
#include "opal/mca/threads/mutex.h"
#include "ompi/mca/mca.h"

#include "ompi/constants.h"
#include "ompi/mca/coll/coll.h"
#include "ompi/mca/coll/base/base.h"
#include "ompi/communicator/communicator.h"

BEGIN_C_DECLS

/*
 * ============================================================================
 * Global Barrier Accelerator Hardware Specifications
 * ============================================================================
 */

/** Maximum number of concurrent barrier groups per switch */
#define GBA_MAX_GROUPS            32

/** Maximum number of members per barrier group (physical ports) */
#define GBA_MAX_MEMBERS           708

/** Number of 64-bit words to represent 708-bit member mask */
#define GBA_MEMBER_MASK_WORDS     ((GBA_MAX_MEMBERS + 63) / 64)  /* 12 words */

/*
 * ============================================================================
 * Hardware Register Map
 * ============================================================================
 *
 * Each barrier group has a dedicated register space in the GBA hardware.
 * Register layout is designed for efficient remote store operations.
 *
 * Base address calculation:
 *   group_base = GBA_GROUP_REG_BASE(group_id)
 *   reg_addr = group_base + reg_offset
 */

/** Register space size per barrier group (4KB aligned) */
#define GBA_GROUP_REG_SIZE        0x1000

/** Calculate base address for a barrier group */
#define GBA_GROUP_REG_BASE(gid)   ((gid) * GBA_GROUP_REG_SIZE)

/* Register offsets within a barrier group */
#define GBA_REG_GROUP_ID          0x0000  /* Group identifier (RO) */
#define GBA_REG_MEMBER_COUNT      0x0004  /* Number of members (RW) */
#define GBA_REG_CONTROL           0x0008  /* Control register (RW) */
#define GBA_REG_STATUS            0x000C  /* Status register (RO) */
#define GBA_REG_ARRIVAL_COUNT     0x0010  /* Arrival count (atomic inc) */
#define GBA_REG_SEQUENCE          0x0014  /* Current barrier sequence (RO) */
#define GBA_REG_MEMBER_MASK_BASE  0x0020  /* Member mask (12 x 64-bit) */
#define GBA_REG_ARRIVED_MASK_BASE 0x0080  /* Arrived mask (12 x 64-bit) (RO) */
#define GBA_REG_RELEASE_FLAG_BASE 0x0100  /* Per-member release flag addresses */

/*
 * Control Register Bits (GBA_REG_CONTROL)
 */
#define GBA_CTRL_ENABLE           (1U << 0)   /* Enable barrier group */
#define GBA_CTRL_RESET            (1U << 1)   /* Reset barrier state */
#define GBA_CTRL_ARM              (1U << 2)   /* Arm for next barrier */

/*
 * Status Register Bits (GBA_REG_STATUS)
 */
#define GBA_STATUS_READY          (1U << 0)   /* Group is ready */
#define GBA_STATUS_ACTIVE         (1U << 1)   /* Barrier in progress */
#define GBA_STATUS_COMPLETE       (1U << 2)   /* All members arrived */

/*
 * ============================================================================
 * Remote Store Message Format
 * ============================================================================
 *
 * The GBA uses remote store semantics for communication:
 * - Arrival: MPI rank stores to GBA's arrival register
 * - Release: GBA stores to each rank's local release flag
 */

/** Remote store arrival message payload */
typedef struct gba_arrival_msg {
    uint32_t group_id;      /* Barrier group ID */
    uint32_t member_id;     /* Member ID within group (0-707) */
    uint32_t sequence;      /* Barrier sequence number */
    uint32_t reserved;      /* Padding */
} gba_arrival_msg_t;

/*
 * ============================================================================
 * Device and Data Structures
 * ============================================================================
 */

/**
 * GBA device handle
 */
typedef struct gba_device {
    void               *base_addr;          /* MMIO base address */
    int                 device_fd;          /* Device file descriptor */
    int                 num_groups;         /* Available groups (32) */
    uint32_t            group_alloc_mask;   /* Bitmask of allocated groups */
    opal_mutex_t        lock;               /* Device access lock */
} gba_device_t;

/**
 * Barrier group configuration
 */
typedef struct gba_group_config {
    uint32_t            group_id;           /* Allocated group ID (0-31) */
    uint32_t            member_count;       /* Number of members in group */
    uint32_t            local_member_id;    /* This rank's member ID */
    uint64_t            member_mask[GBA_MEMBER_MASK_WORDS]; /* 708-bit mask */
} gba_group_config_t;

/**
 * Local state for release polling
 * This memory must be accessible by the GBA via DMA for release stores
 */
typedef struct gba_local_state {
    volatile uint64_t  *release_flag;       /* Flag for release detection */
    uint64_t            expected_seq;       /* Expected sequence value */
    void               *flag_memory;        /* Allocated flag memory */
    size_t              flag_size;          /* Size of flag memory */
    uint64_t            dma_addr;           /* DMA address for GBA writes */
} gba_local_state_t;

/**
 * DMA context for GBA access to host memory
 */
typedef struct gba_dma_context {
    int                 dma_fd;             /* DMA device fd */
    void               *dma_handle;         /* Platform DMA handle */
    opal_mutex_t        lock;               /* DMA context lock */
} gba_dma_context_t;

/*
 * ============================================================================
 * MCA Component Structures
 * ============================================================================
 */

/**
 * Per-communicator module for GBA barrier
 */
typedef struct mca_coll_gba_module_t {
    mca_coll_base_module_t  super;

    /* GBA-specific data */
    gba_device_t           *device;         /* GBA device handle */
    gba_group_config_t      config;         /* Group configuration */
    gba_local_state_t       local_state;    /* Local polling state */
    
    uint32_t                barrier_seq;    /* Current barrier sequence */
    bool                    offload_enabled;/* GBA offload active */

    /* Fallback collective functions */
    mca_coll_base_module_barrier_fn_t    previous_barrier;
    mca_coll_base_module_t              *previous_barrier_module;
    mca_coll_base_module_ibarrier_fn_t   previous_ibarrier;
    mca_coll_base_module_t              *previous_ibarrier_module;
} mca_coll_gba_module_t;

OBJ_CLASS_DECLARATION(mca_coll_gba_module_t);

/**
 * GBA component global data
 */
typedef struct mca_coll_gba_component_t {
    mca_coll_base_component_3_0_0_t  super;

    /* Configuration parameters */
    int                 priority;           /* Component priority */
    int                 disable;            /* Force disable */
    char               *device_path;        /* Device path */
    int                 min_comm_size;      /* Minimum communicator size */
    
    /* Global device state */
    gba_device_t        device;             /* GBA device */
    gba_dma_context_t   dma_ctx;            /* DMA context */
    bool                initialized;        /* Component initialized */
} mca_coll_gba_component_t;

/* Globally exported component */
OMPI_DECLSPEC extern mca_coll_gba_component_t mca_coll_gba_component;

/*
 * ============================================================================
 * Control Plane API Functions
 * ============================================================================
 */

/**
 * Initialize GBA device
 */
int gba_device_init(gba_device_t *device, const char *dev_path);

/**
 * Finalize GBA device
 */
int gba_device_fini(gba_device_t *device);

/**
 * Allocate a barrier group ID
 */
int gba_allocate_group(gba_device_t *device, uint32_t *group_id);

/**
 * Free a barrier group ID
 */
int gba_free_group(gba_device_t *device, uint32_t group_id);

/**
 * Configure a barrier group
 */
int gba_configure_group(gba_device_t *device, gba_group_config_t *config);

/**
 * Read GBA register
 */
int gba_reg_read(gba_device_t *device, uint32_t group_id,
                 uint32_t offset, uint64_t *value);

/**
 * Write GBA register
 */
int gba_reg_write(gba_device_t *device, uint32_t group_id,
                  uint32_t offset, uint64_t value);

/*
 * ============================================================================
 * DMA Context API Functions
 * ============================================================================
 */

/**
 * Initialize DMA context
 */
int gba_dma_init(gba_dma_context_t *ctx, gba_device_t *device);

/**
 * Finalize DMA context
 */
int gba_dma_fini(gba_dma_context_t *ctx);

/**
 * Map memory for GBA DMA access
 */
int gba_dma_map(gba_dma_context_t *ctx, void *vaddr, size_t size,
                uint64_t *dma_addr);

/**
 * Unmap DMA memory
 */
int gba_dma_unmap(gba_dma_context_t *ctx, uint64_t dma_addr);

/*
 * ============================================================================
 * Local State Management
 * ============================================================================
 */

/**
 * Initialize local state with flag memory
 */
int gba_local_state_init(gba_local_state_t *state, gba_dma_context_t *ctx);

/**
 * Finalize local state
 */
int gba_local_state_fini(gba_local_state_t *state, gba_dma_context_t *ctx);

/*
 * ============================================================================
 * Remote Store Operations
 * ============================================================================
 */

/**
 * Send arrival notification to GBA via remote store
 *
 * This function performs a remote store to the GBA's arrival register.
 * The GBA hardware atomically aggregates arrivals from all members.
 *
 * @param[in] device    GBA device handle
 * @param[in] group_id  Barrier group ID
 * @param[in] member_id This rank's member ID
 * @param[in] sequence  Barrier sequence number
 *
 * @return OMPI_SUCCESS on success
 */
static inline int gba_send_arrival(gba_device_t *device,
                                   uint32_t group_id,
                                   uint32_t member_id,
                                   uint32_t sequence)
{
    volatile uint64_t *arrival_reg;
    uint64_t arrival_val;

    /*
     * Construct arrival value:
     *   [63:32] - sequence number
     *   [31:0]  - member ID
     */
    arrival_val = ((uint64_t)sequence << 32) | member_id;

    /* Calculate arrival register address */
    arrival_reg = (volatile uint64_t *)
        ((char *)device->base_addr +
         GBA_GROUP_REG_BASE(group_id) +
         GBA_REG_ARRIVAL_COUNT);

    /*
     * Remote store to GBA arrival register
     * The GBA hardware will:
     * 1. Decode the member_id and set corresponding bit in arrived_mask
     * 2. Increment arrival_count atomically
     * 3. If all members arrived, broadcast release via remote store
     */
    *arrival_reg = arrival_val;

    /* Memory barrier to ensure store completes */
    opal_atomic_wmb();

    return OMPI_SUCCESS;
}

/**
 * Poll local release flag for barrier completion
 *
 * After sending arrival, each rank polls its local release flag.
 * When the GBA detects all members have arrived, it performs
 * a remote store to each member's release flag with the sequence number.
 *
 * @param[in] state    Local state containing release flag
 * @param[in] sequence Expected sequence number
 *
 * @return true if barrier is complete, false otherwise
 */
static inline bool gba_poll_release(gba_local_state_t *state, uint64_t sequence)
{
    /* Memory barrier before reading */
    opal_atomic_rmb();

    /* Check if release flag has been updated by GBA */
    return (*(state->release_flag) >= sequence);
}

/*
 * ============================================================================
 * MCA Component Interface Functions
 * ============================================================================
 */

/**
 * Component initialization query
 */
int mca_coll_gba_init_query(bool enable_progress_threads,
                            bool enable_mpi_threads);

/**
 * Communicator query
 */
mca_coll_base_module_t *mca_coll_gba_comm_query(
    struct ompi_communicator_t *comm, int *priority);

/**
 * Blocking barrier
 */
int mca_coll_gba_barrier(struct ompi_communicator_t *comm,
                         mca_coll_base_module_t *module);

/**
 * Non-blocking barrier
 */
int mca_coll_gba_ibarrier(struct ompi_communicator_t *comm,
                          ompi_request_t **request,
                          mca_coll_base_module_t *module);

END_C_DECLS

#endif /* MCA_COLL_GBA_BARRIER_EXPORT_H */
