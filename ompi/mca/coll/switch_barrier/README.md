# Switch Barrier Accelerator for Open MPI

This document describes the implementation of switch-based barrier accelerator offload for Open MPI. The accelerator is implemented in network switch hardware and provides hardware-accelerated barrier synchronization.

## Overview

The switch barrier accelerator offloads MPI barrier operations to network switch hardware. Key features:

- Hardware aggregation of barrier arrival signals from all group members
- Remote store (non-RDMA) mechanism for barrier signaling
- Local flag polling for barrier release detection
- Support for up to 128 members per barrier group
- Up to 256 barrier groups per switch

## Architecture

```
+----------------+     Remote Store      +-------------------+
|   MPI Process  | ------------------->  |  Switch Barrier   |
|   (Member 0)   |                       |   Accelerator     |
+----------------+                       |                   |
        ^                                |  - Arrival Reg    |
        |                                |  - Member Mask    |
        | Poll Local Flag                |  - Group Config   |
        |                                |                   |
+----------------+                       +-------------------+
| Local Memory   |                              |
| (Release Flag) | <----------------------------+
+----------------+     Broadcast Release
                       (Remote Store)
```

## Components

### 1. Control Plane (`coll_switch_barrier_control_plane.c`)

Handles communication between host and switch hardware:

- Device initialization via MMIO mapping
- Register read/write operations
- Barrier group allocation and configuration
- Member network address registration

Key functions:
- `switch_barrier_control_plane_init()` - Initialize device connection
- `switch_barrier_reg_read/write()` - Register access
- `switch_barrier_configure_group()` - Configure barrier group
- `switch_barrier_allocate_group()` - Allocate group ID

### 2. IOMMU Configuration (`coll_switch_barrier_iommu.c`)

Placeholder for platform-specific IOMMU configuration:

- DMA mapping for switch access to host memory
- Identity mapping used as placeholder
- Supports Linux VFIO, Intel VT-d, ARM SMMU

Key functions:
- `switch_barrier_iommu_init()` - Initialize IOMMU context
- `switch_barrier_iommu_map()` - Map memory for DMA access
- `switch_barrier_iommu_unmap()` - Unmap memory

### 3. MCA Component (`coll_switch_barrier_component.c`)

Open MPI MCA component infrastructure:

- Component registration and parameter handling
- Global device and IOMMU context management
- Priority configuration (default: 90)

MCA Parameters:
- `coll_switch_barrier_priority` - Component priority
- `coll_switch_barrier_disable` - Disable offload
- `coll_switch_barrier_device_path` - Device path
- `coll_switch_barrier_min_comm_size` - Minimum comm size

### 4. Module Implementation (`coll_switch_barrier_module.c`)

Per-communicator module:

- Communication domain initialization
- Barrier group configuration per communicator
- Hardware-accelerated barrier execution
- Fallback to software barrier on failure

## Hardware Register Map

| Offset | Register | Description |
|--------|----------|-------------|
| 0x0000 | NETWORK_ADDR | Network address of switch |
| 0x0008 | GROUP_ID | Barrier group identifier |
| 0x0010 | MEMBER_MASK_LO | Member mask (bits 0-63) |
| 0x0018 | MEMBER_MASK_HI | Member mask (bits 64-127) |
| 0x0020 | MEMBER_COUNT | Number of members |
| 0x0028 | CONTROL | Control register |
| 0x0030 | STATUS | Status register |
| 0x0038 | ARRIVED_MASK_LO | Arrived members (bits 0-63) |
| 0x0040 | ARRIVED_MASK_HI | Arrived members (bits 64-127) |
| 0x0048 | LOCAL_MEMBER_ID | Local member ID |
| 0x0050 | RELEASE_ADDR | Address for release signal |
| 0x0058 | ARRIVAL_ADDR | Address for arrival signal |

### Control Register Bits

| Bit | Name | Description |
|-----|------|-------------|
| 0 | ENABLE | Enable barrier group |
| 1 | RESET | Reset barrier state |
| 2 | ARM | Arm for next barrier |
| 3 | INTERRUPT_EN | Enable completion interrupt |

### Status Register Bits

| Bit | Name | Description |
|-----|------|-------------|
| 0 | READY | Barrier group ready |
| 1 | ACTIVE | Barrier in progress |
| 2 | COMPLETE | All members arrived |
| 3 | ERROR | Error condition |

## Barrier Protocol

### Initialization (Communicator Creation)

1. Allocate barrier group ID from switch
2. Configure group with member count and mask
3. Register network addresses for all members
4. Allocate local flag memory for release polling
5. Map flag memory for DMA via IOMMU
6. Enable barrier group in switch

### Barrier Execution

1. **Arrival Phase**
   - Increment barrier sequence number
   - Send arrival notification via remote store to switch
   - Switch receives arrival, updates arrived mask

2. **Aggregation (in Switch)**
   - Switch compares arrived mask with member mask
   - When all members arrive (arrived == member mask)
   - Broadcast release signal to all members

3. **Release Phase**
   - Switch writes release sequence to each member's flag
   - Members poll local flag for release
   - When flag >= expected sequence, barrier completes

### Remote Store Message Format

```c
struct switch_barrier_remote_store_msg_t {
    uint8_t  msg_type;      // ARRIVE or RELEASE
    uint8_t  reserved[3];
    uint32_t group_id;
    uint32_t member_id;
    uint32_t sequence;
    uint64_t timestamp;
    uint64_t target_addr;
    uint64_t store_value;
};
```

## Usage

### Building

The component is built as part of Open MPI when the switch hardware is available:

```bash
./configure --enable-mca-no-build=coll-switch_barrier  # To disable
./configure  # Enabled by default if hardware detected
make
make install
```

### Runtime Configuration

```bash
# Use switch barrier with high priority
mpirun --mca coll_switch_barrier_priority 100 ./app

# Disable switch barrier
mpirun --mca coll_switch_barrier_disable 1 ./app

# Set device path
mpirun --mca coll_switch_barrier_device_path /dev/switch1 ./app

# Set minimum communicator size
mpirun --mca coll_switch_barrier_min_comm_size 4 ./app
```

## Platform-Specific IOMMU Implementation

The IOMMU module provides placeholders for platform-specific implementation. To add support for a specific platform:

### Linux VFIO

```c
// In switch_barrier_iommu_init():
ctx->iommu_fd = open("/dev/vfio/vfio", O_RDWR);
ioctl(ctx->iommu_fd, VFIO_GET_API_VERSION);
ioctl(ctx->iommu_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

// In switch_barrier_iommu_map():
struct vfio_iommu_type1_dma_map dma_map = {
    .argsz = sizeof(dma_map),
    .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
    .vaddr = (uint64_t)vaddr,
    .iova = allocated_iova,
    .size = size,
};
ioctl(ctx->iommu_fd, VFIO_IOMMU_MAP_DMA, &dma_map);
```

### Intel VT-d

Use kernel IOMMU driver with DMAR table configuration.

### ARM SMMU

Configure stream IDs and context banks per ARM SMMU specification.

## Limitations

- Maximum 128 members per barrier group
- Maximum 256 barrier groups per switch
- Inter-communicators not supported
- Non-blocking ibarrier falls back to software implementation

## File Structure

```
ompi/mca/coll/switch_barrier/
├── coll_switch_barrier.h              # Header with register definitions
├── coll_switch_barrier_component.c    # MCA component
├── coll_switch_barrier_module.c       # Per-comm module
├── coll_switch_barrier_control_plane.c # Control plane interface
├── coll_switch_barrier_iommu.c        # IOMMU configuration
├── Makefile.am                        # Build configuration
├── owner.txt                          # Component ownership
└── README.md                          # This documentation
```

## Future Work

1. Implement non-blocking ibarrier with switch hardware
2. Add support for hierarchical barriers (inter-switch)
3. Performance optimization for small communicators
4. Integration with GPU-aware MPI for GPU barriers
5. Hardware interrupt support for lower latency
