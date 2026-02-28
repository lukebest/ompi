# GBA 控制面初始化流程分析

## 概述

本文档详细描述 Global Barrier Accelerator (GBA) 硬件控制面在 OpenMPI 的 MPI 和 OpenSHMEM 两种运行时中的初始化流程。包括：

1. MPI/OpenSHMEM 原生初始化流程分析
2. GBA 控制面初始化的集成点
3. 代码修改建议

---

## 一、MPI 初始化流程分析

### 1. MPI 初始化调用链

```
用户调用 MPI_Init()
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│  ompi_mpi_init()                                            │
│  文件: ompi/runtime/ompi_mpi_init.c                         │
│                                                             │
│  1. ompi_hook_base_mpi_init_top()                          │
│  2. ompi_mpi_instance_init()                               │
│  3. ompi_comm_init_mpi3()                                  │
│  4. PMIx modex / fence                                     │
│  5. MCA_PML_CALL(add_comm())                               │
│  6. ★ mca_coll_base_comm_select(MPI_COMM_WORLD)  ← COLL选择│
│  7. ★ mca_coll_base_comm_select(MPI_COMM_SELF)             │
│  8. PMIx barrier                                           │
│  9. ompi_mpi_state = INIT_COMPLETED                        │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│  mca_coll_base_comm_select()                                │
│  文件: ompi/mca/coll/base/coll_base_comm_select.c           │
│                                                             │
│  1. check_components() - 遍历所有可用 COLL 组件            │
│  2. query() - 查询每个组件的优先级                         │
│  3. 排序并选择最高优先级组件                               │
│  4. module->coll_module_enable() - 启用选中的模块          │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│  COLL 组件 init_query / comm_query                         │
│  文件: ompi/mca/coll/gba_barrier/coll_gba_barrier_component.c│
│                                                             │
│  1. mca_coll_gba_init_query() - 组件级初始化               │
│     └── gba_device_init() - 初始化 GBA 设备                │
│     └── gba_dma_init() - 初始化 DMA 上下文                 │
│                                                             │
│  2. mca_coll_gba_comm_query() - 每通信器查询               │
│     └── 返回模块和优先级                                   │
└─────────────────────────────────────────────────────────────┘
```

### 2. MPI 关键代码位置

**ompi_mpi_init.c 中的 COLL 选择调用** (约第 580 行):

```c
/* Initialize the coll framework on MPI_COMM_WORLD */
if (OMPI_SUCCESS != (ret = mca_coll_base_comm_select(MPI_COMM_WORLD))) {
    error = "mca_coll_base_comm_select(MPI_COMM_WORLD) failed";
    goto error;
}

/* Initialize the coll framework on MPI_COMM_SELF */
if (OMPI_SUCCESS != (ret = mca_coll_base_comm_select(MPI_COMM_SELF))) {
    error = "mca_coll_base_comm_select(MPI_COMM_SELF) failed";
    goto error;
}
```

### 3. MPI GBA 控制面初始化时机

GBA 控制面初始化应该在 **两个层次** 进行：

| 层次 | 函数 | 时机 | 初始化内容 |
|------|------|------|-----------|
| **组件级** | `mca_coll_gba_init_query()` | MPI_Init 期间，框架打开后 | 设备打开、MMIO 映射、DMA 初始化 |
| **通信器级** | `mca_coll_gba_module_enable()` | 每个通信器创建时 | 分配 barrier group、配置成员、映射 flag |

---

## 二、OpenSHMEM 初始化流程分析

### 1. OpenSHMEM 初始化调用链

```
用户调用 shmem_init() / start_pes()
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│  oshmem_shmem_init()                                        │
│  文件: oshmem/runtime/oshmem_shmem_init.c                   │
│                                                             │
│  1. ompi_mpi_init() - 初始化 MPI 底层                       │
│  2. PMPI_Comm_dup(MPI_COMM_WORLD, &oshmem_comm_world)      │
│  3. _shmem_init() - OpenSHMEM 核心初始化                   │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│  _shmem_init()                                              │
│  文件: oshmem/runtime/oshmem_shmem_init.c                   │
│                                                             │
│  1. oshmem_shmem_register_params()                         │
│  2. oshmem_info_init()                                     │
│  3. oshmem_proc_init()                                     │
│  4. mca_base_framework_open(&oshmem_spml_base_framework)   │
│  5. mca_base_framework_open(&oshmem_scoll_base_framework)  │
│  6. mca_spml_base_select()                                 │
│  7. ★ mca_scoll_base_find_available() - 查找 SCOLL 组件    │
│  8. oshmem_proc_group_init() - 初始化进程组                │
│  9. MCA_SPML_CALL(enable())                                │
│ 10. MCA_SPML_CALL(add_procs())                             │
│ 11. ... (memheap, sshmem, atomic 框架)                     │
│ 12. ★ mca_scoll_enable() - 启用 SCOLL 组件                 │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│  oshmem_proc_group_init()                                   │
│  文件: oshmem/proc/proc.c                                   │
│                                                             │
│  1. oshmem_proc_group_create_nofail(0, 1, size)            │
│     - 创建 oshmem_group_all (所有 PE)                       │
│  2. oshmem_proc_group_create_nofail(0, 1, 1)               │
│     - 创建 oshmem_group_self (当前 PE)                      │
│  3. ★ mca_scoll_base_select(group) - 为组选择 SCOLL        │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│  mca_scoll_base_select()                                    │
│  文件: oshmem/mca/scoll/base/scoll_base_select.c            │
│                                                             │
│  1. check_components() - 遍历 SCOLL 组件                    │
│  2. query() - 查询优先级                                    │
│  3. module->scoll_module_enable() - 启用模块                │
│  4. 安装 barrier/broadcast/collect/reduce/alltoall 函数    │
└─────────────────────────────────────────────────────────────┘
```

### 2. OpenSHMEM 关键代码位置

**_shmem_init() 中的 SCOLL 初始化** (oshmem_shmem_init.c 约第 300-400 行):

```c
/* 打开 SCOLL 框架 */
if (OSHMEM_SUCCESS != (ret = mca_base_framework_open(&oshmem_scoll_base_framework, MCA_BASE_OPEN_DEFAULT))) {
    error = "mca_scoll_base_open() failed";
    goto error;
}

/* 查找可用的 SCOLL 组件 */
if (OSHMEM_SUCCESS != (ret = mca_scoll_base_find_available(OPAL_ENABLE_PROGRESS_THREADS, 1))) {
    error = "mca_scoll_base_find_available() failed";
    goto error;
}

/* ... 稍后 ... */

/* 启用 SCOLL (调用 group init 中的 select) */
if (OSHMEM_SUCCESS != (ret = mca_scoll_enable())) {
    error = "mca_scoll_enable() failed";
    goto error;
}
```

**oshmem_proc_group_init() 中的组创建** (oshmem/proc/proc.c):

```c
int oshmem_proc_group_init(void)
{
    /* 创建所有 PE 的组 */
    oshmem_group_all = oshmem_proc_group_create_nofail(0, 1, oshmem_num_procs());
    
    /* 创建自己的组 */
    oshmem_group_self = oshmem_proc_group_create_nofail(oshmem_my_proc_id(), 1, 1);
    
    return OSHMEM_SUCCESS;
}
```

**mca_scoll_base_select() 中的模块启用** (oshmem/mca/scoll/base/scoll_base_select.c):

```c
int mca_scoll_base_select(struct oshmem_group_t *group)
{
    /* 检查所有可用组件 */
    selectable = check_components(&oshmem_scoll_base_framework.framework_components, group);
    
    /* 选择并启用模块 */
    for (item = opal_list_remove_first(selectable); item != NULL; ...) {
        avail_com_t *avail = (avail_com_t *) item;
        ret = avail->ac_module->scoll_module_enable(avail->ac_module, group);
        
        /* 安装函数指针 */
        COPY(avail->ac_module, group, barrier);
        COPY(avail->ac_module, group, broadcast);
        COPY(avail->ac_module, group, collect);
        COPY(avail->ac_module, group, reduce);
        COPY(avail->ac_module, group, alltoall);
    }
}
```

### 3. OpenSHMEM GBA 控制面初始化时机

| 层次 | 函数 | 时机 | 初始化内容 |
|------|------|------|-----------|
| **组件级** | `mca_scoll_gba_init_query()` | _shmem_init 期间 | 设备打开、MMIO 映射、DMA 初始化 |
| **组级** | `mca_scoll_gba_module_enable()` | 每个组创建时 | 分配 barrier group、配置成员、映射 flag |

---

## 三、GBA 控制面初始化代码修改

### 1. MPI 侧：coll_gba_barrier 组件修改

#### coll_gba_barrier_component.c

```c
/**
 * 组件初始化查询
 * 
 * 此函数在 MPI_Init 期间被 COLL 框架调用一次。
 * 这是 GBA 控制面初始化的正确位置。
 */
int mca_coll_gba_init_query(bool enable_progress_threads,
                            bool enable_mpi_threads)
{
    int ret;

    /* 检查是否被用户禁用 */
    if (mca_coll_gba_component.disable) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: disabled by user");
        return OMPI_ERR_NOT_AVAILABLE;
    }

    /* 已初始化则直接返回 */
    if (mca_coll_gba_component.initialized) {
        return OMPI_SUCCESS;
    }

    /*
     * ★ GBA 控制面初始化 - Step 1: 打开设备
     */
    ret = gba_device_init(&mca_coll_gba_component.device,
                          mca_coll_gba_component.device_path);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: failed to init device %s",
                            mca_coll_gba_component.device_path);
        return ret;
    }

    /*
     * ★ GBA 控制面初始化 - Step 2: 初始化 DMA 上下文
     * 
     * DMA 上下文用于 GBA 向主机内存写入 release flag。
     * 需要建立 IOMMU 映射或物理地址映射。
     */
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
                        "coll:gba_barrier: GBA control plane initialized");
    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier:   device: %s",
                        mca_coll_gba_component.device_path);
    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier:   max_groups: %d",
                        GBA_MAX_GROUPS);
    opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                        "coll:gba_barrier:   max_members: %d",
                        GBA_MAX_MEMBERS);

    return OMPI_SUCCESS;
}

/**
 * 通信器查询
 * 
 * 此函数在每个通信器创建时被调用，决定是否使用 GBA。
 * 返回 NULL 表示不使用 GBA，返回模块表示可以使用。
 */
mca_coll_base_module_t *mca_coll_gba_comm_query(
    struct ompi_communicator_t *comm, int *priority)
{
    mca_coll_gba_module_t *module;
    int comm_size;

    /* 控制面必须已初始化 */
    if (!mca_coll_gba_component.initialized) {
        return NULL;
    }

    /* 不支持跨通信器 */
    if (OMPI_COMM_IS_INTER(comm)) {
        return NULL;
    }

    comm_size = ompi_comm_size(comm);

    /* 检查最小/最大大小限制 */
    if (comm_size < mca_coll_gba_component.min_comm_size ||
        comm_size > GBA_MAX_MEMBERS) {
        return NULL;
    }

    /* 分配模块 */
    module = OBJ_NEW(mca_coll_gba_module_t);
    if (NULL == module) {
        return NULL;
    }

    module->device = &mca_coll_gba_component.device;
    *priority = mca_coll_gba_component.priority;

    /* 设置函数指针 */
    module->super.coll_module_enable = mca_coll_gba_module_enable;
    module->super.coll_module_disable = mca_coll_gba_module_disable;
    module->super.coll_barrier = mca_coll_gba_barrier;
    module->super.coll_ibarrier = mca_coll_gba_ibarrier;

    return &module->super;
}
```

#### coll_gba_barrier_module.c

```c
/**
 * 模块启用
 * 
 * 当 GBA 被选中用于某个通信器时调用。
 * 这是配置 barrier group 的正确位置。
 */
static int mca_coll_gba_module_enable(mca_coll_base_module_t *module,
                                       struct ompi_communicator_t *comm)
{
    mca_coll_gba_module_t *m = (mca_coll_gba_module_t *)module;
    int ret;

    /* 保存之前的 barrier 函数用于 fallback */
    MCA_COLL_SAVE_API(comm, barrier, m->previous_barrier,
                       m->previous_barrier_module, "gba_barrier");
    MCA_COLL_SAVE_API(comm, ibarrier, m->previous_ibarrier,
                       m->previous_ibarrier_module, "gba_barrier");

    /*
     * ★ GBA 控制面初始化 - Step 3: 配置 barrier group
     * 
     * 为此通信器分配一个 GBA group ID 并配置硬件。
     */
    ret = gba_configure_comm_domain(m, comm);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(10, ompi_coll_base_framework.framework_output,
                            "coll:gba_barrier: failed to configure, using fallback");
        /* 继续使用 fallback，但不返回错误 */
    }

    /* 安装 GBA 函数 */
    MCA_COLL_INSTALL_API(comm, barrier, mca_coll_gba_barrier,
                          &m->super, "gba_barrier");
    MCA_COLL_INSTALL_API(comm, ibarrier, mca_coll_gba_ibarrier,
                          &m->super, "gba_barrier");

    return OMPI_SUCCESS;
}

/**
 * 配置通信域
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

    /* 检查硬件限制 */
    if (comm_size > GBA_MAX_MEMBERS) {
        return OMPI_ERR_NOT_SUPPORTED;
    }

    /*
     * ★ GBA 控制面操作: 分配 barrier group
     */
    ret = gba_allocate_group(module->device, &group_id);
    if (OMPI_SUCCESS != ret) {
        return ret;
    }

    /* 配置组参数 */
    module->config.group_id = group_id;
    module->config.member_count = comm_size;
    module->config.local_member_id = my_rank;

    /* 构建 member mask (708 位) */
    memset(module->config.member_mask, 0, sizeof(module->config.member_mask));
    for (i = 0; i < comm_size; i++) {
        int word_idx = i / 64;
        int bit_idx = i % 64;
        module->config.member_mask[word_idx] |= (1ULL << bit_idx);
    }

    /*
     * ★ GBA 控制面操作: 初始化本地状态
     * 
     * 分配 release flag 内存并建立 DMA 映射
     */
    ret = gba_local_state_init(&module->local_state,
                               &mca_coll_gba_component.dma_ctx);
    if (OMPI_SUCCESS != ret) {
        gba_free_group(module->device, group_id);
        return ret;
    }

    /*
     * ★ GBA 控制面操作: 配置硬件组
     */
    ret = gba_configure_group(module->device, &module->config);
    if (OMPI_SUCCESS != ret) {
        gba_local_state_fini(&module->local_state,
                             &mca_coll_gba_component.dma_ctx);
        gba_free_group(module->device, group_id);
        return ret;
    }

    module->barrier_seq = 0;
    module->offload_enabled = true;

    return OMPI_SUCCESS;
}
```

### 2. OpenSHMEM 侧：scoll_gba 组件修改

#### scoll_gba_component.c

```c
/**
 * SCOLL 组件初始化查询
 * 
 * 此函数在 _shmem_init 期间被 SCOLL 框架调用。
 * 与 MPI 侧类似，这是 GBA 控制面初始化的位置。
 */
int mca_scoll_gba_init_query(bool enable_progress_threads,
                              bool enable_mpi_threads)
{
    int ret;

    if (mca_scoll_gba_component.disable) {
        return OSHMEM_ERR_NOT_AVAILABLE;
    }

    if (mca_scoll_gba_component.initialized) {
        return OSHMEM_SUCCESS;
    }

    /*
     * ★ GBA 控制面初始化
     * 
     * 注意：如果 MPI 侧已经初始化过，这里可以复用。
     * 检查组件是否已初始化，避免重复初始化。
     */
    ret = gba_device_init(&mca_scoll_gba_component.device,
                          mca_scoll_gba_component.device_path);
    if (OSHMEM_SUCCESS != ret) {
        return ret;
    }

    ret = gba_dma_init(&mca_scoll_gba_component.dma_ctx,
                       &mca_scoll_gba_component.device);
    if (OSHMEM_SUCCESS != ret) {
        gba_device_fini(&mca_scoll_gba_component.device);
        return ret;
    }

    mca_scoll_gba_component.initialized = true;
    return OSHMEM_SUCCESS;
}

/**
 * 组查询
 * 
 * 在每个 oshmem_group 创建时调用 (oshmem_proc_group_init)
 */
mca_scoll_base_module_t *mca_scoll_gba_comm_query(
    oshmem_group_t *group, int *priority)
{
    mca_scoll_gba_module_t *module;

    if (!mca_scoll_gba_component.initialized) {
        return NULL;
    }

    /* 检查组大小限制 */
    if (group->proc_count > GBA_MAX_MEMBERS) {
        return NULL;
    }

    module = OBJ_NEW(mca_scoll_gba_module_t);
    if (NULL == module) {
        return NULL;
    }

    module->device = &mca_scoll_gba_component.device;
    *priority = mca_scoll_gba_component.priority;

    module->super.scoll_module_enable = mca_scoll_gba_module_enable;
    module->super.scoll_barrier = mca_scoll_gba_barrier;

    return &module->super;
}
```

#### scoll_gba_module.c

```c
/**
 * 模块启用
 * 
 * 当 GBA 被选中用于某个 oshmem_group 时调用
 */
int mca_scoll_gba_module_enable(mca_scoll_base_module_t *module,
                                 oshmem_group_t *group)
{
    mca_scoll_gba_module_t *m = (mca_scoll_gba_module_t *)module;
    int ret;

    /* 保存之前的函数 */
    if (group->g_scoll.scoll_barrier_module != NULL) {
        m->previous_barrier = group->g_scoll.scoll_barrier;
        m->previous_barrier_module = group->g_scoll.scoll_barrier_module;
    }

    /*
     * ★ GBA 控制面: 配置 barrier group
     */
    ret = gba_configure_group_for_oshmem(m, group);
    if (OSHMEM_SUCCESS != ret) {
        /* 使用 fallback */
        return OSHMEM_SUCCESS;
    }

    /* 安装 GBA barrier */
    group->g_scoll.scoll_barrier = mca_scoll_gba_barrier;
    group->g_scoll.scoll_barrier_module = module;
    OBJ_RETAIN(module);

    return OSHMEM_SUCCESS;
}

/**
 * 配置 oshmem group
 */
static int gba_configure_group_for_oshmem(mca_scoll_gba_module_t *module,
                                           oshmem_group_t *group)
{
    int ret;
    uint32_t group_id;
    int i;

    if (group->proc_count > GBA_MAX_MEMBERS) {
        return OSHMEM_ERR_NOT_SUPPORTED;
    }

    /* 分配 GBA group */
    ret = gba_allocate_group(module->device, &group_id);
    if (OSHMEM_SUCCESS != ret) {
        return ret;
    }

    module->config.group_id = group_id;
    module->config.member_count = group->proc_count;
    module->config.local_member_id = group->my_pe;

    /* 构建 member mask */
    memset(module->config.member_mask, 0, sizeof(module->config.member_mask));
    for (i = 0; i < group->proc_count; i++) {
        int word_idx = i / 64;
        int bit_idx = i % 64;
        module->config.member_mask[word_idx] |= (1ULL << bit_idx);
    }

    /* 初始化本地状态 */
    ret = gba_local_state_init(&module->local_state,
                               &mca_scoll_gba_component.dma_ctx);
    if (OSHMEM_SUCCESS != ret) {
        gba_free_group(module->device, group_id);
        return ret;
    }

    /* 配置硬件 */
    ret = gba_configure_group(module->device, &module->config);
    if (OSHMEM_SUCCESS != ret) {
        gba_local_state_fini(&module->local_state,
                             &mca_scoll_gba_component.dma_ctx);
        gba_free_group(module->device, group_id);
        return ret;
    }

    module->barrier_seq = 0;
    module->offload_enabled = true;

    return OSHMEM_SUCCESS;
}
```

---

## 四、控制面初始化流程图

### MPI 侧流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                         MPI_Init()                                   │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  ompi_mpi_init()                                                     │
│  ─────────────────                                                   │
│  • ompi_mpi_instance_init()                                          │
│  • ompi_comm_init_mpi3()                                             │
│  • PMIx modex / fence                                                │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  mca_coll_base_comm_select(MPI_COMM_WORLD)                           │
│  ─────────────────────────────────────                               │
│  • 遍历所有 COLL 组件                                                │
│  • 调用每个组件的 init_query()                                       │
│  • 调用每个组件的 comm_query()                                       │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  mca_coll_gba_init_query()          ← ★ GBA 组件级初始化            │
│  ──────────────────────────                                          │
│  • gba_device_init()                                                 │
│    - open("/dev/gba0")                                               │
│    - mmap(MMIO space)                                                │
│  • gba_dma_init()                                                    │
│    - 初始化 DMA/IOMMU 上下文                                         │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  mca_coll_gba_comm_query()          ← 返回模块和优先级              │
│  ───────────────────────────                                         │
│  • 检查通信器大小限制                                                │
│  • 分配 mca_coll_gba_module_t                                        │
│  • 设置函数指针                                                      │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼ (如果 GBA 被选中)
┌─────────────────────────────────────────────────────────────────────┐
│  mca_coll_gba_module_enable()       ← ★ GBA 通信器级初始化         │
│  ─────────────────────────────                                       │
│  • gba_allocate_group()                                              │
│    - 分配 barrier group ID (0-31)                                    │
│  • gba_local_state_init()                                            │
│    - 分配 release flag 内存                                          │
│    - 建立 DMA 映射                                                   │
│  • gba_configure_group()                                             │
│    - 写入 member_mask 到硬件                                         │
│    - 使能 barrier group                                              │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  MPI_Init() 完成，可以调用 MPI_Barrier()                             │
│                                                                      │
│  MPI_Barrier(MPI_COMM_WORLD)                                         │
│    → mca_coll_gba_barrier()                                          │
│      → gba_send_arrival()     // 远程 Store 到 GBA                  │
│      → gba_poll_release()     // 轮询本地 flag                      │
└─────────────────────────────────────────────────────────────────────┘
```

### OpenSHMEM 侧流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                       shmem_init()                                   │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  oshmem_shmem_init()                                                 │
│  ───────────────────                                                 │
│  • ompi_mpi_init()          // MPI 底层初始化                       │
│  • _shmem_init()            // OpenSHMEM 核心初始化                 │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  _shmem_init()                                                       │
│  ────────────                                                        │
│  • mca_base_framework_open(&oshmem_scoll_base_framework)            │
│  • mca_scoll_base_find_available()                                   │
│    → 调用每个 SCOLL 组件的 init_query()                              │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  mca_scoll_gba_init_query()         ← ★ GBA 组件级初始化            │
│  ─────────────────────────                                           │
│  • gba_device_init()                                                 │
│  • gba_dma_init()                                                    │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  oshmem_proc_group_init()                                            │
│  ───────────────────────                                             │
│  • oshmem_proc_group_create_nofail(0, 1, size)  // group_all        │
│  • oshmem_proc_group_create_nofail(my_pe, 1, 1) // group_self       │
│    → 每个组创建时调用 mca_scoll_base_select()                        │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  mca_scoll_base_select(group)                                        │
│  ─────────────────────────                                           │
│  • check_components()                                                │
│  • 调用每个组件的 query()                                            │
│  • 如果选中，调用 module_enable()                                    │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  mca_scoll_gba_module_enable()      ← ★ GBA 组级初始化             │
│  ───────────────────────────                                         │
│  • gba_allocate_group()                                              │
│  • gba_local_state_init()                                            │
│  • gba_configure_group()                                             │
│  • 安装 group->g_scoll.scoll_barrier                                 │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  shmem_init() 完成，可以调用 shmem_barrier()                         │
│                                                                      │
│  shmem_barrier_all()                                                 │
│    → mca_scoll_gba_barrier()                                         │
│      → gba_send_arrival()     // 远程 Store 到 GBA                  │
│      → gba_poll_release()     // 轮询本地 flag                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 五、共享 GBA 设备的考虑

当 MPI 和 OpenSHMEM 同时使用时（如在 MPI + OpenSHMEM 混合程序中），需要考虑 GBA 设备的共享：

### 方案 1: 全局单例设备

```c
/* 在公共头文件中定义 */
typedef struct gba_global_device {
    gba_device_t        device;
    gba_dma_context_t   dma_ctx;
    opal_mutex_t        lock;
    int                 refcount;
    bool                initialized;
} gba_global_device_t;

/* 全局单例 */
extern gba_global_device_t gba_global_device;

/* 初始化函数 - 线程安全，只初始化一次 */
int gba_global_device_init(const char *device_path);

/* 引用计数增加/减少 */
void gba_global_device_acquire(void);
void gba_global_device_release(void);
```

### 方案 2: MPI 和 OpenSHMEM 独立设备

```c
/* MPI 使用 */
gba_device_init(&mca_coll_gba_component.device, "/dev/gba0");

/* OpenSHMEM 使用 (如果 MPI 未初始化) */
if (!gba_global_device.initialized) {
    gba_device_init(&mca_scoll_gba_component.device, "/dev/gba0");
}
```

---

## 六、资源清理流程

### MPI Finalize 流程

```c
/* coll_gba_barrier_component.c */
static int gba_component_close(void)
{
    if (mca_coll_gba_component.initialized) {
        gba_dma_fini(&mca_coll_gba_component.dma_ctx);
        gba_device_fini(&mca_coll_gba_component.device);
        mca_coll_gba_component.initialized = false;
    }
    return OMPI_SUCCESS;
}

/* coll_gba_barrier_module.c */
static void mca_coll_gba_module_destruct(mca_coll_gba_module_t *module)
{
    if (module->offload_enabled && module->device != NULL) {
        gba_free_group(module->device, module->config.group_id);
        gba_local_state_fini(&module->local_state,
                             &mca_coll_gba_component.dma_ctx);
    }
}
```

### OpenSHMEM Finalize 流程

```c
/* oshmem/runtime/oshmem_shmem_finalize.c */
int oshmem_shmem_finalize(void)
{
    /* 释放所有组的 SCOLL 资源 */
    oshmem_proc_group_finalize_scoll();
    
    /* 关闭 SCOLL 框架 (调用组件的 close()) */
    mca_base_framework_close(&oshmem_scoll_base_framework);
    
    ...
}
```

---

## 七、关键文件列表

### MPI 侧

| 文件 | 描述 |
|------|------|
| `ompi/runtime/ompi_mpi_init.c` | MPI 初始化入口 |
| `ompi/mca/coll/base/coll_base_comm_select.c` | COLL 组件选择 |
| `ompi/mca/coll/gba_barrier/coll_gba_barrier.h` | GBA 头文件 |
| `ompi/mca/coll/gba_barrier/coll_gba_barrier_component.c` | GBA 组件注册 |
| `ompi/mca/coll/gba_barrier/coll_gba_barrier_module.c` | GBA 模块实现 |
| `ompi/mca/coll/gba_barrier/coll_gba_barrier_control.c` | GBA 控制面 |

### OpenSHMEM 侧

| 文件 | 描述 |
|------|------|
| `oshmem/runtime/oshmem_shmem_init.c` | OpenSHMEM 初始化入口 |
| `oshmem/proc/proc.c` | 进程组创建 |
| `oshmem/mca/scoll/base/scoll_base_select.c` | SCOLL 组件选择 |
| `oshmem/mca/scoll/gba/scoll_gba_component.c` | GBA SCOLL 组件 |
| `oshmem/mca/scoll/gba/scoll_gba_module.c` | GBA SCOLL 模块 |

---

## 八、配置参数汇总

### MPI 侧 MCA 参数

```bash
# GBA 优先级 (默认 100)
--mca coll_gba_barrier_priority 100

# 禁用 GBA
--mca coll_gba_barrier_disable 1

# 设备路径
--mca coll_gba_barrier_device_path /dev/gba0

# 最小通信器大小
--mca coll_gba_barrier_min_comm_size 2
```

### OpenSHMEM 侧 MCA 参数

```bash
# GBA SCOLL 优先级
--mca scoll_gba_priority 100

# 禁用 GBA
--mca scoll_gba_disable 1

# 设备路径
--mca scoll_gba_device_path /dev/gba0
```
