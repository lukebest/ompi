# Global Barrier Accelerator 对接分析

## 概述

本文档分析 OpenMPI 中 **OpenSHMEM** barrier 原语的实现方式，以及如何与交换机 Global Barrier Accelerator 硬件对接。

> **MPI Barrier 对接**: 如果您需要了解 **MPI Barrier** 与 GBA 的对接方式，请参考 [gba_barrier_mpi_integration.md](./gba_barrier_mpi_integration.md)，该文档包含完整的 MPI Barrier 实现分析和 GBA 硬件对接代码。

---


## 概述

本文档分析 OpenMPI 中 OpenSHMEM barrier 原语的实现方式，以及如何与交换机 Global Barrier Accelerator 硬件对接。

---

## 一、OpenSHMEM Barrier 原语实现方式

### 1. 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                    User Application                          │
│              shmem_barrier() / shmem_barrier_all()           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                 oshmem/shmem/c/shmem_barrier.c               │
│            (API wrapper, group creation, quiet)              │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│          SCOLL (SHMEM Collective) Layer                      │
│       oshmem/mca/scoll/basic/scoll_basic_barrier.c           │
│     多种算法: Central Counter, Tournament, Recursive         │
│     Doubling, Dissemination, Basic, Adaptive                 │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│               SPML (SHMEM Point-to-point) Layer              │
│          oshmem/mca/spml/ucx/spml_ucx.c                      │
│        put/get/wait 操作 (远程存储语义)                       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│              UCX / BTL (网络传输层)                           │
│           InfiniBand, RoCE, TCP等                            │
└─────────────────────────────────────────────────────────────┘
```

### 2. 关键数据结构

**oshmem_group_t** (进程组):

```c
struct oshmem_group_t {
    int                         id;             // 全局数组索引
    int                         my_pe;          // 本PE编号
    int                         proc_count;     // 组内进程数
    int                         is_member;      // 是否组成员
    opal_vpid_t                 *proc_vpids;    // 每个进程的vpid
    mca_scoll_base_group_scoll_t g_scoll;       // 集合操作模块
    ompi_communicator_t*        ompi_comm;      // MPI通信子
};
```

### 3. Barrier 算法实现

`scoll_basic_barrier.c` 实现了多种算法:

| 算法 | 复杂度 | 特点 |
|------|--------|------|
| **Central Counter** | O(N) | Root轮询所有PE的pSync状态，然后广播释放 |
| **Tournament** | O(log N) | 锦标赛式两两同步，适合共享内存 |
| **Recursive Doubling** | O(log N) | 成对交换+递归倍增 |
| **Dissemination** | O(log N) | 改进的蝴蝶barrier，支持非2的幂次PE数 |
| **Basic** | O(N) | 使用send/recv，适合同节点小规模 |
| **Adaptive** | 自适应 | 根据PE数量和位置选择算法 |

**核心实现模式 (以Recursive Doubling为例)**:

```c
static int _algorithm_recursive_doubling(oshmem_group_t *group, long *pSync)
{
    // 1. 设置本地pSync为WAIT状态
    // 2. 在每轮中，与peer_id = my_id ^ (1 << round)的PE交换
    // 3. 使用 SPML put/get 原语进行远程同步
    // 4. 本地wait等待pSync变为特定值
    // 5. 恢复pSync初始值
    
    // 关键操作:
    MCA_SPML_CALL(get(ctx, pSync, sizeof(value), &value, peer_pe));
    MCA_SPML_CALL(put(ctx, pSync, sizeof(value), &value, peer_pe));
    MCA_SPML_CALL(wait(pSync, SHMEM_CMP_EQ, &value, SHMEM_LONG));
}
```

### 4. SPML层的put/get实现

SPML UCX模块通过UCX库实现远程存储语义:

```c
// put操作 - 远程写入
int mca_spml_ucx_put(shmem_ctx_t ctx, void* dst_addr, 
                     size_t size, void* src_addr, int dst);

// get操作 - 远程读取  
int mca_spml_ucx_get(shmem_ctx_t ctx, void* dst_addr,
                     size_t size, void* src_addr, int src);

// wait操作 - 本地轮询
int mca_spml_ucx_wait(void* addr, int cmp, void* value, int datatype);
```

---

## 二、现有参考实现：switch_barrier 组件分析

### 1. 组件位置

```
ompi/mca/coll/switch_barrier/
├── coll_switch_barrier.h              # 寄存器定义、数据结构
├── coll_switch_barrier_component.c    # MCA组件注册
├── coll_switch_barrier_module.c       # 每通信域模块
├── coll_switch_barrier_control_plane.c # 硬件控制接口
├── coll_switch_barrier_iommu.c        # IOMMU/DMA映射
├── Makefile.am                        # 构建配置
└── README.md                          # 完整文档
```

### 2. 硬件特性对比

| 特性 | Global Barrier Accelerator | 现有 switch_barrier |
|------|---------------------------|-------------------|
| 最大 barrier groups | 32 | 256 |
| 每组最大 members | 708 | 128 |
| 远程 store 语义 | ✓ | ✓ |
| 本地 flag 轮询 | ✓ | ✓ |
| 框架位置 | OpenSHMEM (SCOLL) | MPI (COLL) |

### 3. 架构图

```
                    交换机 Global Barrier Accelerator
                    ┌────────────────────────────────┐
                    │   Group 0: 128 ports           │
                    │   Group 1: 128 ports           │
                    │   ...                          │
                    │   Group 255: 128 ports         │
                    │                                │
                    │   每个Group:                    │
                    │   - arrive_count (原子聚合)    │
                    │   - release_flags[] (广播)     │
                    └────────────────────────────────┘
                         ▲              │
            Remote Store │              │ Remote Store
            (Arrive)     │              │ (Release)
                         │              ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│     PE 0     │    │     PE 1     │    │    PE N-1    │
│              │    │              │    │              │
│ local_flag   │    │ local_flag   │    │ local_flag   │
│ ┌──────────┐ │    │ ┌──────────┐ │    │ ┌──────────┐ │
│ │  value   │◄├────┤ │  value   │◄├────┤ │  value   │ │
│ └──────────┘ │    │ └──────────┘ │    │ └──────────┘ │
│   spin wait  │    │   spin wait  │    │   spin wait  │
└──────────────┘    └──────────────┘    └──────────────┘
```

### 4. 硬件寄存器布局

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

### 5. 关键数据结构

```c
/* Switch barrier accelerator device handle */
typedef struct switch_barrier_device_t {
    void *base_addr;                     /* Base address for MMIO access */
    void *control_plane_handle;          /* Control plane connection handle */
    uint64_t network_addr;               /* Network address of this switch */
    int device_fd;                       /* Device file descriptor */
    int num_groups;                      /* Number of available barrier groups */
    uint64_t group_allocation_mask;      /* Bitmask of allocated groups */
    opal_mutex_t lock;                   /* Device access lock */
} switch_barrier_device_t;

/* Barrier group configuration */
typedef struct switch_barrier_group_config_t {
    uint32_t group_id;                   /* Barrier group identifier */
    uint32_t member_count;               /* Number of members */
    uint64_t member_mask[2];             /* 128-bit member mask */
    uint64_t network_addrs[SWITCH_BARRIER_MAX_MEMBERS]; /* Member network addresses */
    uint32_t local_member_id;            /* This node's member ID in the group */
} switch_barrier_group_config_t;

/* Local barrier state for flag polling */
typedef struct switch_barrier_local_state_t {
    volatile uint64_t *arrival_flag;     /* Local flag for arrival signal */
    volatile uint64_t *release_flag;     /* Local flag for release signal */
    uint64_t expected_sequence;          /* Expected sequence number for release */
    void *flag_memory;                   /* Allocated memory for flags */
    size_t flag_memory_size;             /* Size of flag memory */
    uint64_t iommu_iova;                 /* IOVA for IOMMU mapping */
} switch_barrier_local_state_t;

/* Remote store message structure */
typedef struct switch_barrier_remote_store_msg_t {
    uint8_t  msg_type;                   /* ARRIVE or RELEASE */
    uint8_t  reserved[3];
    uint32_t group_id;
    uint32_t member_id;
    uint32_t sequence;
    uint64_t timestamp;
    uint64_t target_addr;
    uint64_t store_value;
} switch_barrier_remote_store_msg_t;
```

### 6. Barrier 协议流程

#### 初始化阶段 (Communicator 创建)

1. 分配 barrier group ID
2. 配置 group (member count, mask)
3. 注册所有 member 的网络地址
4. 分配本地 flag 内存
5. 通过 IOMMU 映射 flag 内存
6. 使能 barrier group

#### Barrier 执行阶段

```
1. Arrival Phase:
   - 递增 barrier sequence number
   - 发送 arrival notification (远程 store) 到交换机
   - 交换机接收 arrival，更新 arrived mask

2. Aggregation (在交换机中):
   - 交换机比较 arrived mask 与 member mask
   - 当所有 member 到达 (arrived == member mask)
   - 广播 release 信号到所有 member

3. Release Phase:
   - 交换机向每个 member 的 flag 写入 release sequence
   - Member 本地轮询 flag
   - 当 flag >= expected sequence，barrier 完成
```

### 7. 核心代码实现

**Barrier 执行** (`coll_switch_barrier_module.c`):

```c
int mca_coll_switch_barrier_barrier(struct ompi_communicator_t *comm,
                                    mca_coll_base_module_t *module)
{
    mca_coll_switch_barrier_module_t *s = (mca_coll_switch_barrier_module_t *)module;
    uint32_t sequence;
    int ret;

    // Fallback检查
    if (!s->offload_enabled) {
        return s->c_coll.coll_barrier(comm, s->c_coll.coll_barrier_module);
    }

    // 递增序列号
    sequence = ++s->barrier_sequence;

    // Step 1: 发送Arrival信号到交换机
    ret = switch_barrier_send_arrival(s->device,
                                       s->group_config.group_id,
                                       s->group_config.local_member_id,
                                       sequence);
    if (OMPI_SUCCESS != ret) {
        return s->c_coll.coll_barrier(comm, s->c_coll.coll_barrier_module);
    }

    // Step 2: 本地轮询等待Release
    while (!switch_barrier_poll_release(&s->local_state, sequence)) {
        opal_progress();
    }

    return MPI_SUCCESS;
}
```

**本地Flag轮询** (`coll_switch_barrier.h`):

```c
static inline bool switch_barrier_poll_release(
    switch_barrier_local_state_t *local_state,
    uint64_t sequence)
{
    opal_atomic_rmb();  // 内存屏障
    return (*(local_state->release_flag) >= sequence);
}
```

**发送Arrival信号** (`coll_switch_barrier_control_plane.c`):

```c
int switch_barrier_send_arrival(switch_barrier_device_t *device,
                                uint32_t group_id,
                                uint32_t member_id,
                                uint32_t sequence)
{
    switch_barrier_remote_store_msg_t msg;
    volatile uint64_t *arrival_reg;
    uint64_t arrival_value;

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

    return OMPI_SUCCESS;
}
```

---

## 三、OpenSHMEM SCOLL 对接方案

### 1. 创建 GBA 组件

基于现有的 `switch_barrier` 模式，创建针对 Global Barrier Accelerator 硬件的 OpenSHMEM 版本:

```
oshmem/mca/scoll/gba/
├── scoll_gba.h                 # 头文件（适配708端口/32组）
├── scoll_gba_component.c       # MCA组件
├── scoll_gba_module.c          # 每group模块
├── scoll_gba_control.c         # 硬件控制接口
├── scoll_gba_barrier.c         # Barrier实现
├── Makefile.am
└── README.md
```

### 2. 硬件参数定义

```c
// scoll_gba.h

#ifndef MCA_SCOLL_GBA_H
#define MCA_SCOLL_GBA_H

#include "oshmem_config.h"
#include "oshmem/mca/scoll/scoll.h"

BEGIN_C_DECLS

/* Global Barrier Accelerator 硬件参数 */
#define GBA_MAX_GROUPS            32      /* 32个并发barrier group */
#define GBA_MAX_MEMBERS           708     /* 708个物理端口 */

/* 寄存器偏移定义 */
#define GBA_REG_GROUP_ID          0x0000
#define GBA_REG_MEMBER_COUNT      0x0004
#define GBA_REG_ARRIVAL_COUNT     0x0008
#define GBA_REG_MEMBER_MASK_BASE  0x0010  /* 708位 = 12个64位寄存器 */
#define GBA_REG_RELEASE_ADDR_BASE 0x0080  /* 每成员release地址 */
#define GBA_REG_CONTROL           0x1000
#define GBA_REG_STATUS            0x1008

/* Control register bits */
#define GBA_CTRL_ENABLE           (1ULL << 0)
#define GBA_CTRL_RESET            (1ULL << 1)
#define GBA_CTRL_ARM              (1ULL << 2)

/* Status register bits */
#define GBA_STATUS_READY          (1ULL << 0)
#define GBA_STATUS_ACTIVE         (1ULL << 1)
#define GBA_STATUS_COMPLETE       (1ULL << 2)

/* Remote store message types */
#define GBA_MSG_ARRIVE            0x01
#define GBA_MSG_RELEASE           0x02

/* Device handle */
typedef struct gba_device_t {
    void *base_addr;
    int device_fd;
    int num_groups;
    uint32_t group_allocation_mask;
    opal_mutex_t lock;
} gba_device_t;

/* Group configuration */
typedef struct gba_group_config_t {
    uint32_t group_id;
    uint32_t member_count;
    uint64_t member_mask[12];           /* 708位 */
    uint32_t local_member_id;
} gba_group_config_t;

/* Local state */
typedef struct gba_local_state_t {
    volatile uint64_t *release_flag;
    uint64_t expected_sequence;
    void *flag_memory;
    size_t flag_memory_size;
} gba_local_state_t;

/* Module structure */
typedef struct mca_scoll_gba_module_t {
    mca_scoll_base_module_t super;
    
    gba_device_t *device;
    gba_group_config_t group_config;
    gba_local_state_t local_state;
    
    uint32_t barrier_sequence;
    bool offload_enabled;
    
    /* Fallback functions */
    mca_scoll_base_module_barrier_fn_t previous_barrier;
    mca_scoll_base_module_t *previous_barrier_module;
} mca_scoll_gba_module_t;

/* Component structure */
typedef struct mca_scoll_gba_component_t {
    mca_scoll_base_component_1_0_0_t super;
    
    int priority;
    int disable;
    char *device_path;
    int min_group_size;
    
    gba_device_t device;
    bool initialized;
} mca_scoll_gba_component_t;

/* API functions */
int mca_scoll_gba_barrier(struct oshmem_group_t *group, 
                          long *pSync, int alg);

int gba_send_arrival(gba_device_t *device,
                     uint32_t group_id,
                     uint32_t member_id,
                     uint32_t sequence);

static inline bool gba_poll_release(gba_local_state_t *state, uint64_t seq)
{
    opal_atomic_rmb();
    return (*(state->release_flag) >= seq);
}

/* Globally exported component */
OSHMEM_DECLSPEC extern mca_scoll_gba_component_t mca_scoll_gba_component;

END_C_DECLS

#endif /* MCA_SCOLL_GBA_H */
```

### 3. Barrier 实现代码

```c
// scoll_gba_barrier.c

#include "scoll_gba.h"
#include "oshmem/mca/spml/spml.h"
#include "oshmem/proc/proc.h"

int mca_scoll_gba_barrier(struct oshmem_group_t *group, 
                          long *pSync, int alg)
{
    mca_scoll_gba_module_t *gba_module;
    uint32_t sequence;
    int gba_group_id;
    int ret;

    gba_module = (mca_scoll_gba_module_t *)
        group->g_scoll.scoll_barrier_module;

    /* 检查硬件是否可用 */
    if (!gba_module->offload_enabled) {
        /* Fallback到basic实现 */
        return gba_module->previous_barrier(group, pSync, alg);
    }

    /* 检查组大小限制 */
    if (group->proc_count > GBA_MAX_MEMBERS) {
        return gba_module->previous_barrier(group, pSync, alg);
    }

    /* 获取GBA group ID */
    gba_group_id = gba_module->group_config.group_id;
    sequence = ++gba_module->barrier_sequence;

    /*
     * Step 1: 发送Arrival信号
     * 通过远程store语义向交换机GBA写入到达信息
     */
    ret = gba_send_arrival(gba_module->device,
                           gba_group_id,
                           gba_module->group_config.local_member_id,
                           sequence);
    if (OSHMEM_SUCCESS != ret) {
        return gba_module->previous_barrier(group, pSync, alg);
    }

    /*
     * Step 2: 本地轮询等待Release
     * 交换机GBA在收集完所有member的arrival后，
     * 会向每个member的local_flag执行远程store写入
     */
    while (!gba_poll_release(&gba_module->local_state, sequence)) {
        opal_progress();
    }

    return OSHMEM_SUCCESS;
}

int gba_send_arrival(gba_device_t *device,
                     uint32_t group_id,
                     uint32_t member_id,
                     uint32_t sequence)
{
    volatile uint64_t *arrival_reg;
    uint64_t arrival_value;

    /* 构造arrival值 */
    arrival_value = ((uint64_t)member_id << 32) | sequence;

    /* 写入GBA的arrival寄存器（远程store语义）*/
    arrival_reg = (volatile uint64_t *)
        ((char *)device->base_addr +
         GBA_GROUP_REG_BASE(group_id) +
         GBA_REG_ARRIVAL);

    *arrival_reg = arrival_value;
    opal_atomic_wmb();

    return OSHMEM_SUCCESS;
}
```

### 4. 组件注册代码

```c
// scoll_gba_component.c

#include "scoll_gba.h"

static int gba_register(void);

mca_scoll_gba_component_t mca_scoll_gba_component = {
    {
        .scoll_version = {
            MCA_SCOLL_BASE_VERSION_2_0_0,
            .mca_component_name = "gba",
            MCA_BASE_MAKE_VERSION(component, OMPI_MAJOR_VERSION, OMPI_MINOR_VERSION,
                                  OMPI_RELEASE_VERSION),
            .mca_register_component_params = gba_register,
        },
        .scoll_data = {0},
        .scoll_init = mca_scoll_gba_init,
        .scoll_query = mca_scoll_gba_query,
    },
    
    .priority = 100,
    .disable = 0,
    .device_path = "/dev/gba0",
    .min_group_size = 2,
    .initialized = false,
};

static int gba_register(void)
{
    mca_scoll_gba_component.priority = 100;
    (void) mca_base_component_var_register(
        &mca_scoll_gba_component.super.scoll_version,
        "priority",
        "Priority of GBA scoll component",
        MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
        OPAL_INFO_LVL_6,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_scoll_gba_component.priority);

    mca_scoll_gba_component.disable = 0;
    (void) mca_base_component_var_register(
        &mca_scoll_gba_component.super.scoll_version,
        "disable",
        "Disable GBA hardware acceleration",
        MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
        OPAL_INFO_LVL_2,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_scoll_gba_component.disable);

    mca_scoll_gba_component.device_path = "/dev/gba0";
    (void) mca_base_component_var_register(
        &mca_scoll_gba_component.super.scoll_version,
        "device_path",
        "Path to GBA device",
        MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
        OPAL_INFO_LVL_4,
        MCA_BASE_VAR_SCOPE_READONLY,
        &mca_scoll_gba_component.device_path);

    return OSHMEM_SUCCESS;
}
```

---

## 四、配置与使用

### 1. 构建配置

```bash
# 配置OpenMPI，启用GBA组件
./configure --enable-scoll-gba \
            --with-gba=/path/to/gba/library
make && make install
```

### 2. 运行时配置

```bash
# 使用GBA硬件加速
export OMPI_MCA_scoll_gba_priority=100
export OMPI_MCA_scoll_gba_device_path=/dev/gba0

# 禁用GBA
export OMPI_MCA_scoll_gba_disable=1

# 运行OpenSHMEM程序
oshrun -np 708 ./your_shmem_app
```

### 3. 性能对比

| 实现方式 | 延迟 | 网络跳数 | 适用场景 |
|---------|------|---------|---------|
| Basic (send/recv) | O(N) | O(N²) | 小规模、本地 |
| Recursive Doubling | O(log N) | O(N log N) | 大规模分布式 |
| **GBA硬件加速** | **O(1)** | **O(N)** | 708 PE以内的集群 |

---

## 五、总结

1. **现有参考**：代码库中的 `ompi/mca/coll/switch_barrier` 提供了完整的硬件加速 barrier 实现模板

2. **OpenSHMEM 对接**：创建 `oshmem/mca/scoll/gba` 组件，遵循 SCOLL 框架模式

3. **核心机制**：
   - **Arrival**: 远程 store 到交换机 GBA
   - **Aggregation**: 交换机硬件聚合
   - **Release**: 交换机远程 store 到各端点的本地 flag
   - **Detection**: 端点本地轮询 flag 检测完成

4. **性能优势**：
   - 延迟从 O(log N) 降低到 O(1)
   - 单跳网络延迟
   - 硬件聚合，无软件开销

---

## 六、参考文件

| 文件 | 描述 |
|------|------|
| `ompi/mca/coll/switch_barrier/README.md` | 硬件加速 barrier 完整文档 |
| `ompi/mca/coll/switch_barrier/coll_switch_barrier.h` | 寄存器定义、数据结构 |
| `ompi/mca/coll/switch_barrier/coll_switch_barrier_component.c` | MCA组件注册 |
| `ompi/mca/coll/switch_barrier/coll_switch_barrier_module.c` | 每通信域模块 |
| `ompi/mca/coll/switch_barrier/coll_switch_barrier_control_plane.c` | 硬件控制接口 |
| `oshmem/mca/scoll/scoll.h` | SCOLL 框架接口 |
| `oshmem/mca/scoll/basic/scoll_basic_barrier.c` | 软件 barrier 实现 |
