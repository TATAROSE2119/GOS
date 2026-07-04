# GOS `do_fork()` 与 COW 内存变化全过程图解

> 本文档严格参考 GOS 项目源码 `user/user.c`（第 383-439 行）、`mm/cow.c`、
> `include/gos/user.h`、`include/asm/pgtable.h`，
> 用内存框图逐步还原 `do_fork()` 执行期间 VA（虚拟地址）、PA（物理地址）、
> 页表（PGD/PMD/PTE）的每一次变化。每个箭头上标注的是**实际触发该变化的项目函数**。

---

## 背景知识速查

### GOS 用户空间地址范围 (`include/gos/user.h` 第 27-34 行)
| 宏定义 | 值 | 含义 |
|:---|:---|:---|
| `USER_SPACE_CODE_START` | `0x1000` | 用户代码起始地址 |
| `USER_SPACE_FIXED_MMAP` | `0x0` | 固定映射区起始 |
| `USER_SPACE_FIXED_MMAP_SIZE` | `1GB` | 固定映射区大小 |
| `USER_SPACE_TOTAL_SIZE` | `4GB` | 用户空间总大小 |

### RISC-V PTE 权限位定义 (`include/asm/pgtable.h` 第 122-131 行)
| 位 | 宏名 | bit | 含义 |
|:---:|:---|:---:|:---|
| 0 | `_PAGE_PRESENT` | bit[0] | 页表项有效 |
| 1 | `_PAGE_READ` | bit[1] | 可读 |
| 2 | `_PAGE_WRITE` | bit[2] | 可写 |
| 3 | `_PAGE_EXEC` | bit[3] | 可执行 |
| 4 | `_PAGE_USER` | bit[4] | 用户态可访问 |
| 5 | `_PAGE_GLOBAL` | bit[5] | 全局页（不随 ASID 刷新） |
| 6 | `_PAGE_ACCESSED` | bit[6] | 已被访问（硬件自动置位） |
| 7 | `_PAGE_DIRTY` | bit[7] | 已被写入（硬件自动置位） |
| 8 | `_PAGE_SOFT` | bit[8] | 软件保留位 |
| 9 | `_PAGE_COW` | bit[9] | **写时复制标记（GOS 自定义）** |

### PTE 操作函数 (`include/asm/pgtable.h` 第 182-199 行)
| 函数 | 代码 | 作用 |
|:---|:---|:---|
| `pte_wrprotect(pte)` | `return pte & ~_PAGE_WRITE` | 清除 bit[2]，剥夺写权限 |
| `pte_mkcow(pte)` | `return pte \| _PAGE_COW` | 置位 bit[9]，标记 COW |
| `pte_is_cow(pte)` | `return !!(pte & _PAGE_COW)` | 检查是否为 COW 页 |
| `pte_uncow_mkwrite(pte)` | `return (pte & ~_PAGE_COW) \| _PAGE_WRITE \| _PAGE_DIRTY` | 清 COW，恢复写权限 |

### `struct user` 关键字段 (`include/gos/user.h` 第 179-195 行)
```
struct user {
    struct list_head list;               // 挂在 per_cpu(user_list) 上
    int user_id;                         // 进程 PID
    struct user_mode_cpu_context cpu_context; // 包含 s_context(内核态) + u_context(用户态)
    unsigned long user_code_va;          // 用户代码段的内核虚拟地址
    unsigned long user_code_pa;          // 用户代码段的物理地址
    unsigned long user_code_user_va;     // 用户代码段的用户虚拟地址 (0x1000)
    unsigned long user_share_memory_va;  // 共享内存的内核虚拟地址
    unsigned long user_share_memory_pa;  // 共享内存的物理地址
    unsigned long user_share_memory_user_va; // 共享内存的用户虚拟地址
    spinlock_t lock;
    struct list_head memory_region;      // 用户空间内存区域链表
    int mapping;                         // 是否已完成映射
    void *pgdp;                          // 本进程的顶级页表基址 (PA)
};
```

### 页表树结构（以 Sv39 为例, `PGDIR_SHIFT = 30`）
```
                    PGD (顶级页表, 4KB, 512项)
                    ├── PGD[0] ─→ PMD (中间级页表, 4KB, 512项)
                    │              ├── PMD[n] ─→ PTE (最底层页表, 4KB, 512项)
                    │              │              ├── PTE[m] ─→ 物理页 PA (4KB数据)
                    │              │              └── ...
                    │              └── ...
                    ├── PGD[1] ─→ (1GB 内核映射)
                    └── ...
    每一级: 512项 × 8字节 = 4KB = 1个物理页
    VA 的拆分: [PGD索引 9bit][PMD索引 9bit][PTE索引 9bit][页内偏移 12bit]
```

---

## 阶段 0：fork 之前 —— 只有父进程

```mermaid
graph TB
    subgraph parent_va ["父进程 VA 空间 (4GB 用户区 + 内核区)"]
        direction TB
        PK["VA >0x80000000 内核区<br/>════════════════════<br/>内核代码 .text (R+X)<br/>内核数据 .data (R+W)<br/>内核堆 mm_alloc 的地盘 (R+W)<br/>  └─ parent->pgdp 在这里<br/>  └─ 父 PMD / PTE 页表树在这里<br/>════════════════════<br/>📌 phy_to_virt() 线性映射<br/>   VA = PA + va_pa_offset"]
        PU["VA 0x1000 ~ 4GB 用户区<br/>════════════════════<br/>0x1000: .text 用户代码段<br/>  PTE 权限: R+X+U (bit 1,3,4)<br/>0x1000+: .data/.bss 数据段<br/>  PTE 权限: R+W+U (bit 1,2,4)<br/>更高地址: heap 堆 / stack 栈<br/>  PTE 权限: R+W+U (bit 1,2,4)<br/>════════════════════<br/>📌 所有用户 VA 通过页表<br/>   翻译到 PA"]
        PSH["VA 0x0 ~ 0x1000 共享内存区<br/>════════════════════<br/>由 user_share_memory_user_va 管理<br/>  PTE 权限: R+W+U (bit 1,2,4)<br/>📌 COW_SHARE_END = 0x1000<br/>   此区域 fork 后不做 COW"]
    end

    subgraph pa_mem ["物理内存 PA (真实的内存条)"]
        direction TB
        PA_KERN["PA 内核区<br/>════════════════════<br/>内核代码 PA (所有进程共享)<br/>内核数据 PA<br/>────────────────<br/>📦 parent->pgdp (4KB 页表)<br/>📦 父 PMD 页表 (4KB × N)<br/>📦 父 PTE 页表 (4KB × N)"]
        PA_CODE["PA 用户代码页<br/>════════════════════<br/>存放编译后的用户程序指令<br/>Ref Count = 1"]
        PA_DATA["PA 用户数据页 (可能多个)<br/>════════════════════<br/>存放全局变量/堆/栈数据<br/>PTE 权限: R+W+U<br/>Ref Count = 1"]
        PA_SHARE["PA 共享内存页<br/>════════════════════<br/>Ref Count = 1"]
    end

    PK ===|"线性映射<br/>phy_to_virt()"| PA_KERN
    PU -->|"父 PTE 映射<br/>bit: V+R+X+U"| PA_CODE
    PU -->|"父 PTE 映射<br/>bit: V+R+W+U"| PA_DATA
    PSH -->|"父 PTE 映射<br/>bit: V+R+W+U"| PA_SHARE
```

---

## 阶段 1：`user_create_force()` —— 分配子进程 PCB

> **代码**: `user/user.c` 第 390 行 → `__user_create()` 第 88-121 行
>
> **调用链**: `user_create_force()` → `__user_create()` → `mm_alloc(sizeof(struct user))` → `memset(清零)` → 初始化 `sstatus`, `memory_region`, `lock` → 加入 `per_cpu(user_list)`

```mermaid
graph TB
    subgraph before ["PA: 分配前"]
        PA_KERN_B["PA 内核区<br/>════════════<br/>(已有父进程数据)"]
    end

    action["__user_create() 内部操作:<br/>════════════════════════<br/>1. mm_alloc(sizeof(struct user))<br/>   从物理内存池切出一块内存<br/>2. memset(清零整个结构体)<br/>3. u_context->sstatus = read_csr(CSR_SSTATUS)<br/>   u_context->sstatus &= ~SR_SPP (清除S位,标记U-Mode)<br/>   u_context->sstatus |= SR_SPIE (开启中断)<br/>4. INIT_LIST_HEAD(&user->memory_region)<br/>5. __SPINLOCK_INIT(&user->lock)<br/>6. list_add_tail(&user->list, per_cpu(user_list))"]

    subgraph after ["PA: 分配后"]
        PA_KERN_A["PA 内核区<br/>════════════<br/>(已有父进程数据)"]
        PA_CHILD_PCB["🆕 PA: struct user (child)<br/>════════════════════════<br/>user_id: 0 (未分配)<br/>cpu_context: sstatus=U-Mode, 其余=0<br/>user_code_va/pa/user_va: 0<br/>memory_region: 空链表<br/>mapping: 0<br/>pgdp: NULL<br/>════════════════════════<br/>📌 内核通过 phy_to_virt(PA)<br/>   得到内核 VA 指针 child"]
    end

    before --> action --> after
```

紧接着 `user_update_userid(child)` (`user/user.c` 第 394 行):
- 调用 `find_free_userid(&userid_bitmap)` 扫描全局位图
- 找到第一个为 0 的 bit 位（例如 bit[2]），将其置 1
- `child->user_id = 2`（此值即为 fork 最终返回给父进程的 PID）

---

## 阶段 2：`mm_alloc(PAGE_SIZE)` + `memcpy` —— 建立子进程 PGD

> **代码**: `user/user.c` 第 396-400 行

```mermaid
graph TB
    subgraph sources ["PA: 两个数据源"]
        PA_DEFAULT["PA: default_pgd<br/>═══════════════<br/>get_default_pgd() 获取的物理地址<br/>phy_to_virt() 转为内核 VA<br/>────────────────<br/>内容: 系统最原始的内核页表<br/>PGD[0]: 指向内核低地址 PMD<br/>PGD[1]: 指向内核 1-2GB 映射<br/>PGD[2...]: 其他内核映射<br/>═══════════════<br/>📌 不包含任何用户空间映射"]
    end

    action1["mm_alloc(PAGE_SIZE):<br/>═══════════════════<br/>从物理内存池切出<br/>连续的 4096 字节 (4KB)<br/>返回: phy_to_virt(PA) 内核 VA"]

    action2["memcpy(child_pgdp, default_pgd, PAGE_SIZE):<br/>═══════════════════════════════<br/>将 default_pgd 的全部 512 个表项<br/>逐字节拷贝到 child_pgdp<br/>────────────────<br/>⚠️ 注意: memcpy 是对内核VA操作<br/>   因为 C 代码只能用 VA 读写内存"]

    subgraph result ["PA: 新增子 PGD"]
        PA_CHILD_PGD["🆕 PA: child_pgdp (4KB)<br/>═══════════════════════<br/>PGD[0]: 与 default_pgd 相同<br/>  (⚠️ 指向父/内核共享的下级表!<br/>   下一步 copy_page_range 会处理)<br/>PGD[1]: 与 default_pgd 相同<br/>  (指向内核 1-2GB 映射)<br/>PGD[2...]: 与 default_pgd 相同<br/>═══════════════════════<br/>📌 child_pgdp 变量 = 内核高位 VA<br/>📌 对应的 PA 将来要写入 satp 寄存器"]
    end

    sources --> action1
    action1 --> action2
    action2 --> result
```

**为什么 PGD[0] 此时很危险？**
> `user/user.c` 第 402-407 行注释原文:
> "memcpy 继承了内核 PGD 的低地址映射(含内核代码 0x80200000 与用户区,
> 二者同在 PGD[0])。copy_page_range 会在遍历用户区时把与父共享的下级
> 页表逐级克隆成子私有表(保留内核等兄弟项)，从而只隔离用户区、不破坏内核映射。"

---

## 阶段 3：`copy_page_range()` —— COW 的核心魔法

> **代码**: `user/user.c` 第 408-418 行 → `mm/cow.c` 第 102-112 行
>
> 此阶段分为两个子步骤：3a 克隆中间级页表，3b 处理叶子 PTE。

### 3a. `copy_level()` 递归克隆页表树

> **代码**: `mm/cow.c` 第 34-96 行

遍历 `parent->memory_region` 链表中的每一个区域 `[region->start, region->end)`，
对每个区域调用 `copy_page_range(start, end, child_pgdp, parent_pgdp)`。

```mermaid
graph TB
    subgraph before ["PA 页表树: copy_level 之前"]
        direction TB
        P_PGD["父 PGD<br/>═══════════<br/>PGD[0] ──→"]
        P_PMD["父 PMD (4KB)<br/>═══════════<br/>PMD[n] ──→"]
        P_PTE["父 PTE (4KB)<br/>═══════════<br/>PTE[m]:<br/>  PFN=PA_DATA>>12<br/>  bit: V+R+W+U<br/>  COW=0"]
        PA_DATA["PA 用户数据页<br/>Ref Count = 1"]

        C_PGD_B["子 PGD<br/>═══════════<br/>PGD[0]: 与父PGD[0]<br/>指向同一个 PMD!<br/>(memcpy继承的)"]

        P_PGD --> P_PMD
        P_PMD --> P_PTE
        P_PTE --> PA_DATA
        C_PGD_B -.->|"⚠️ 危险: 共享!"| P_PMD
    end

    action["copy_level() 递归处理:<br/>══════════════════════════<br/>发现 dst_tbl[0] == src_pte (cow.c 第70行)<br/>即子 PGD[0] 和父 PGD[0] 指向同一个 PMD<br/>────────────────<br/>1. pa = alloc_zero_page(0)  分配新 4KB<br/>2. memcpy(new_PMD, 父PMD, 4KB) 完整拷贝<br/>3. dst_tbl[0] = (pa >> 12) << 10 | _PAGE_PRESENT<br/>   子 PGD[0] 改指新 PMD<br/>────────────────<br/>继续递归下一级: 对子 PMD 的每一项<br/>也执行相同的克隆逻辑"]

    subgraph after_tree ["PA 页表树: copy_level 之后"]
        direction TB
        P_PGD2["父 PGD"]
        P_PMD2["父 PMD (不变)"]
        P_PTE2["父 PTE (即将被修改)"]

        C_PGD_A["子 PGD<br/>═══════════<br/>PGD[0]: 已改指新 PMD ✅"]
        C_PMD["🆕 子 PMD (4KB)<br/>═══════════<br/>alloc_zero_page() 分配<br/>memcpy 拷贝父 PMD 内容<br/>(保留了内核的兄弟项!)"]
        C_PTE["🆕 子 PTE (4KB)<br/>═══════════<br/>alloc_zero_page() 分配<br/>(即将被 copy_leaf 填写)"]

        P_PGD2 --> P_PMD2
        P_PMD2 --> P_PTE2
        C_PGD_A --> C_PMD
        C_PMD --> C_PTE
    end

    before --> action --> after_tree
```

### 3b. `copy_leaf()` 处理叶子 PTE —— COW 标记的核心

> **代码**: `mm/cow.c` 第 15-31 行

当递归到最底层 (`shift == PAGE_SHIFT`, 即 `shift == 12`)，进入 `copy_leaf()`:

```mermaid
graph LR
    subgraph pte_before ["copy_leaf 之前"]
        direction TB
        SRC_B["父 PTE[m] 内容:<br/>══════════════<br/>bit[63:10]: PFN (物理页帧号)<br/>  → 指向 PA_DATA<br/>bit[9]: COW = 0<br/>bit[7]: DIRTY = 1<br/>bit[4]: USER = 1<br/>bit[2]: WRITE = 1 ✅ 可写<br/>bit[1]: READ = 1<br/>bit[0]: VALID = 1"]
        DST_B["子 PTE[m] 内容:<br/>══════════════<br/>(全零, 尚未填写)"]
        PA_B["PA_DATA<br/>Ref Count = 1"]
        SRC_B --> PA_B
    end

    subgraph branch ["copy_leaf 分支判断"]
        direction TB
        CHK["if va < COW_SHARE_END (0x1000)?<br/>══════════════════════<br/>YES → 共享区: *dst_ptep = pte<br/>  (直接拷贝, 保持可写, 不做COW)<br/>────────────────<br/>NO → 匿名区: 执行 COW 标记 ↓"]
        COW_OP["COW 操作 (cow.c 第26-30行):<br/>══════════════════════<br/>cow = pte_mkcow(pte_wrprotect(pte))<br/>  → 清除 bit[2] (WRITE=0)<br/>  → 置位 bit[9] (COW=1)<br/>────────────────<br/>*src_ptep = cow  ⚡修改父 PTE!<br/>*dst_ptep = cow  写入子 PTE<br/>get_page(pa)     引用计数+1"]
        CHK --> COW_OP
    end

    subgraph pte_after ["copy_leaf 之后"]
        direction TB
        SRC_A["父 PTE[m] 内容:<br/>══════════════<br/>bit[63:10]: PFN (不变)<br/>  → 仍指向 PA_DATA<br/>bit[9]: COW = 1 🔴<br/>bit[2]: WRITE = 0 🔴 不可写了!<br/>bit[1]: READ = 1<br/>bit[0]: VALID = 1"]
        DST_A["子 PTE[m] 内容:<br/>══════════════<br/>bit[63:10]: PFN (与父相同!)<br/>  → 也指向 PA_DATA<br/>bit[9]: COW = 1 🔴<br/>bit[2]: WRITE = 0 🔴<br/>bit[1]: READ = 1<br/>bit[0]: VALID = 1"]
        PA_A["PA_DATA<br/>Ref Count = 2 🔴<br/>(父+子共享)"]
        SRC_A --> PA_A
        DST_A --> PA_A
    end

    pte_before --> branch --> pte_after
```

### 3c. 收尾: `add_user_space_memory()` + `local_flush_tlb_range()`

> **代码**: `user/user.c` 第 416-417 行, `mm/cow.c` 第 110 行

```mermaid
graph LR
    A["add_user_space_memory(child, start, size)<br/>══════════════════════════<br/>在 child->memory_region 链表中<br/>新增一个 user_memory_region 节点:<br/>  .start = region->start<br/>  .end = region->start + size<br/>📌 mm_alloc 分配链表节点 (内核堆)"] --> B["local_flush_tlb_range(start, end-start, PAGE_SIZE)<br/>══════════════════════════<br/>刷新父进程的 TLB 缓存<br/>因为父 PTE 的 W 位被改了,<br/>如果不刷 TLB, MMU 可能用<br/>缓存的旧权限(可写)放行写入,<br/>导致 COW 失效!"]
```

---

## 阶段 4：伪造 CPU 上下文

> **代码**: `user/user.c` 第 420-431 行

```mermaid
graph LR
    subgraph parent_ctx ["父进程 cpu_context (ecall 瞬间的快照)"]
        direction TB
        P_CTX["struct user_mode_cpu_context:<br/>══════════════════<br/>u_context.a0 = __NR_fork (syscall 编号)<br/>u_context.a7 = 4 (fork 的 nr_sys)<br/>u_context.sepc = ecall 指令的地址<br/>u_context.sp = 用户态栈顶<br/>u_context.s0~s11 = 用户态寄存器<br/>u_context.sstatus = SPP=0 (U-Mode)<br/>s_context = 内核态寄存器快照"]
    end

    action["操作 (user.c 第420-431行):<br/>══════════════════════════<br/>① child->cpu_context = parent->cpu_context<br/>   (结构体整体赋值, 全盘克隆)<br/>────────────────<br/>② child->cpu_context.u_context.a0 = 0<br/>   篡改 a0 → 子进程 fork() 返回 0<br/>────────────────<br/>③ child->...sepc = parent->...sepc + 4<br/>   PC 加 4 字节 → 跳过 ecall 指令<br/>   (RISC-V 指令长度 = 4 bytes)<br/>────────────────<br/>④ 继承元数据:<br/>   child->user_code_user_va = parent->...<br/>   child->user_code_pa = parent->...<br/>   child->user_share_memory_* = parent->...<br/>   child->mapping = 1"]

    subgraph child_ctx ["子进程 cpu_context (伪造后)"]
        direction TB
        C_CTX["struct user_mode_cpu_context:<br/>══════════════════<br/>u_context.a0 = 0 🔧<br/>  (子进程 fork 返回值)<br/>u_context.a7 = 4 (继承)<br/>u_context.sepc = ecall地址+4 🔧<br/>  (从 ecall 的下一条指令恢复)<br/>u_context.sp = 与父相同<br/>  (指向同一个用户栈VA,<br/>   但 PA 已被 COW 保护)<br/>u_context.s0~s11 = 与父相同<br/>s_context = 与父相同"]
    end

    parent_ctx --> action --> child_ctx
```

---

## 阶段 5：`create_task()` —— 注册到调度器

> **代码**: `user/user.c` 第 433-438 行

```mermaid
graph TB
    action["create_task 参数:<br/>══════════════════<br/>name = 'user_child'<br/>func = user_child_start (入口函数)<br/>data = (void *)child (传递 struct user 指针)<br/>cpu = sbi_get_cpu_id() (当前CPU)<br/>pgdp = virt_to_phy(child_pgdp)<br/>  📌 关键! 将内核 VA 转为纯 PA<br/>  📌 PA 将来写入 satp 寄存器供 MMU 使用"]

    subgraph kernel_alloc ["PA 新增分配"]
        direction TB
        TASK["🆕 struct task (内核堆)<br/>══════════════════<br/>task->name = 'user_child'<br/>task->entry = user_child_start<br/>task->data = child<br/>task->pgdp = virt_to_phy(child_pgdp)<br/>task->state = READY"]
        KSTACK["🆕 内核栈 Kernel Stack (内核堆)<br/>══════════════════<br/>大小: STACK_SIZE<br/>用途: 子进程陷入内核态时<br/>  使用的独立栈空间<br/>  (不与父进程共享!)"]
    end

    subgraph sched ["调度器就绪队列"]
        RQ["RunQueue<br/>══════════════<br/>...其他任务...<br/>→ 🆕 user_child 任务<br/>  入口: user_child_start<br/>  页表: child_pgdp (PA)<br/>  状态: READY<br/>══════════════<br/>📌 等待时钟中断触发调度<br/>📌 被选中后执行 user_child_start"]
    end

    action --> TASK
    action --> KSTACK
    TASK --> RQ

    ret["do_fork() 返回 child->user_id<br/>══════════════════<br/>此返回值最终通过 syscall_handler<br/>写入父进程的 regs->a0<br/>父进程回到用户态后:<br/>  pid = fork(); // pid = child->user_id (非零)"]
    RQ -.-> ret
```

---

## 阶段 6：`do_fork` 完成后 —— 父子内存全景

```mermaid
graph TB
    subgraph parent_va ["父进程 VA 空间"]
        direction TB
        PK["内核区 >0x80000000<br/>════════════════<br/>内核代码/数据<br/>parent->pgdp (父页表树)<br/>🆕 child (子PCB 也在内核堆)<br/>🆕 child_pgdp (子页表树也在内核堆)<br/>🆕 子 PMD/PTE (alloc_zero_page)<br/>🆕 struct task (子任务控制块)<br/>🆕 子内核栈"]
        PU[".text 用户代码 (R+X+U)"]
        PD[".data/heap/stack<br/>⚠️ PTE: R+U (W位被清除!)<br/>⚠️ PTE: COW位=1<br/>📌 写任何数据都会触发<br/>   EXC_STORE_PAGE_FAULT"]
        PSH["0x0~0x1000 共享内存<br/>PTE: R+W+U (不做COW)"]
    end

    subgraph child_va ["子进程 VA 空间"]
        direction TB
        CK["内核区 >0x80000000<br/>════════════════<br/>与父进程完全相同<br/>(memcpy default_pgd 继承)"]
        CU[".text 用户代码 (R+X+U)"]
        CD[".data/heap/stack<br/>⚠️ PTE: R+U (只读!)<br/>⚠️ PTE: COW位=1<br/>📌 写任何数据都会触发<br/>   EXC_STORE_PAGE_FAULT"]
        CSH["0x0~0x1000 共享内存<br/>PTE: R+W+U<br/>(copy_leaf 直接拷贝PTE,<br/> 与父完全共享, 可写)"]
    end

    subgraph pa_mem ["物理内存 PA"]
        direction TB
        PA_K["内核物理区<br/>════════════════<br/>内核代码/数据 PA<br/>parent->pgdp PA (4KB)<br/>🆕 child_pgdp PA (4KB)<br/>🆕 子 PMD PA (4KB × N)<br/>🆕 子 PTE PA (4KB × N)<br/>🆕 child struct user PA<br/>🆕 struct task PA<br/>🆕 子内核栈 PA"]
        PA_CODE["用户代码物理页 PA<br/>Ref Count = 2"]
        PA_DATA["用户数据物理页 PA<br/>════════════════<br/>Ref Count = 2<br/>⚠️ 父子共享同一物理页!<br/>⚠️ 父PTE: W=0, COW=1<br/>⚠️ 子PTE: W=0, COW=1"]
        PA_SHARE["共享内存物理页 PA<br/>════════════════<br/>Ref Count = 2<br/>父PTE: R+W+U (可写)<br/>子PTE: R+W+U (可写)<br/>📌 直接共享, 不做 COW"]
    end

    PK ===|"线性映射"| PA_K
    CK ===|"线性映射"| PA_K
    PU -->|"父 PTE"| PA_CODE
    CU -->|"子 PTE"| PA_CODE
    PD -.->|"父 PTE: R only + COW"| PA_DATA
    CD -.->|"子 PTE: R only + COW"| PA_DATA
    PSH -->|"父 PTE: R+W"| PA_SHARE
    CSH -->|"子 PTE: R+W"| PA_SHARE
```

---

## 阶段 7：COW 触发 —— 子进程写入数据时的内存分裂

> 子进程被调度器选中 → 进入 `user_child_start()` (user.c 第355行)
> → `user_mode_switch_to()` 切入用户态 → 用户程序执行 `a[0] = 1;`

### 7a. 异常触发与路由

```mermaid
graph TB
    subgraph user_mode ["用户态 (U-Mode)"]
        W["子进程执行: a[0] = 1<br/>════════════════<br/>CPU 发出 Store 请求<br/>MMU 查 子PTE: W=0, COW=1<br/>════════════════<br/>⚡ 硬件触发异常<br/>scause = 15 (EXC_STORE_PAGE_FAULT)<br/>sbadaddr = a[0] 的虚拟地址"]
    end

    subgraph trap_chain ["内核态异常处理链"]
        direction TB
        T1["__user_mode_switch_return<br/>═══════════════<br/>保存用户态全部寄存器到 cpu_context<br/>恢复内核态寄存器<br/>ret 回到 C 代码"]
        T2["do_user_exception() (user_exception.c)<br/>═══════════════<br/>scause & (1UL << 63) == 0 → 是异常<br/>switch(scause):<br/>  case EXC_STORE_PAGE_FAULT:<br/>    → cow_try_handle_store(sbadaddr)"]
        T3["cow_try_handle_store(addr) (cow.c 第168行)<br/>═══════════════<br/>pgdp = phy_to_virt(get_current_pgd())<br/>ptep = walk_to_leaf(pgdp, addr)<br/>  → 沿 PGD→PMD→PTE 逐级查找<br/>  → 返回指向叶子 PTE 的指针<br/>────────────────<br/>检查: pte_is_valid 且 pte_is_cow<br/>  且 !(_PAGE_WRITE)<br/>→ 确认是 COW 缺页, 调用 cow_handle_write"]
        T4["cow_handle_write(addr, ptep) (cow.c 第114行)"]
        T1 --> T2 --> T3 --> T4
    end

    W --> T1
```

### 7b. `cow_handle_write()` 内部操作

> **代码**: `mm/cow.c` 第 114-141 行

```mermaid
graph TB
    subgraph check ["判断: 该物理页是否被共享?"]
        CHK["old_pa = pfn_to_phys(pte_pfn(*ptep))<br/>════════════════════<br/>page_count(old_pa) == ?"]
        CASE1["== 1: 独占页 (父进程已经分裂走了)<br/>════════════════════<br/>set_pte(ptep, pte_uncow_mkwrite(pte))<br/>  → 清 COW 位 (bit9=0)<br/>  → 恢复 W 位 (bit2=1)<br/>  → 设 DIRTY 位 (bit7=1)<br/>════════════════════<br/>📌 无需拷贝! 直接恢复写权限"]
        CASE2["== 2: 共享页 (父子都在用)<br/>════════════════════<br/>必须执行完整的 COW 拷贝 ↓"]
    end

    subgraph cow_copy ["共享页的 COW 拷贝流程"]
        direction TB
        S1["mm_alloc(PAGE_SIZE)<br/>════════════════<br/>从内核物理内存池<br/>分配全新 4KB 物理页<br/>返回 new_va (内核高位VA)"]
        S2["memcpy(new_va, phy_to_virt(old_pa), PAGE_SIZE)<br/>════════════════<br/>🔥 全流程中第一次真正的数据拷贝!<br/>将旧物理页的 4096 字节<br/>完整复制到新物理页"]
        S3["new_pa = virt_to_phy(new_va)<br/>════════════════<br/>将新页的内核 VA 转为 PA<br/>用于构造新的 PTE"]
        S4["newpte = (pte & ~_PAGE_PFN_MASK)<br/>       | ((new_pa >> 12) << 10)<br/>════════════════<br/>保留旧 PTE 的权限位<br/>替换 PFN 字段为新物理页"]
        S5["set_pte(ptep, pte_uncow_mkwrite(newpte))<br/>════════════════<br/>写入子进程 PTE:<br/>  PFN → 指向 new_pa (新物理页)<br/>  COW = 0 (清除)<br/>  WRITE = 1 (恢复可写!)<br/>  DIRTY = 1"]
        S6["put_page(old_pa)<br/>════════════════<br/>旧物理页 Ref Count: 2 → 1<br/>📌 父进程仍独占旧页"]
        S7["local_flush_tlb_page_asid(addr, asid)<br/>════════════════<br/>刷新该虚拟地址的 TLB 缓存<br/>确保 MMU 下次用新 PTE"]
        S1 --> S2 --> S3 --> S4 --> S5 --> S6 --> S7
    end

    CHK -->|"page_count == 1"| CASE1
    CHK -->|"page_count >= 2"| CASE2
    CASE2 --> S1
```

### 7c. COW 完成后的内存状态

```mermaid
graph TB
    subgraph parent_va ["父进程 VA 空间"]
        PD["用户数据区<br/>══════════<br/>父 PTE 状态:<br/>  PFN → PA_OLD<br/>  W = 0, COW = 1<br/>────────────<br/>📌 但 PA_OLD 的 Ref=1<br/>📌 当父进程也写入时,<br/>  cow_handle_write 中<br/>  page_count==1 分支<br/>  → 直接恢复 W 权限<br/>  → 无需拷贝!"]
    end

    subgraph child_va ["子进程 VA 空间"]
        CD["用户数据区<br/>══════════<br/>子 PTE 状态:<br/>  PFN → PA_NEW ✅<br/>  W = 1 ✅ 可写!<br/>  COW = 0 ✅ 已清除<br/>────────────<br/>📌 子进程现在可以<br/>  自由读写此地址"]
    end

    subgraph pa_mem ["物理内存 PA"]
        PA_OLD["PA_OLD 旧数据页<br/>══════════════<br/>Ref Count = 1 (put_page 后)<br/>归属: 父进程独占<br/>内容: 原始数据 (未被修改)"]
        PA_NEW["🆕 PA_NEW 新数据页<br/>══════════════<br/>Ref Count = 1<br/>归属: 子进程独占<br/>内容: memcpy(PA_OLD) 的副本<br/>  + 子进程刚写入的 a[0]=1"]
    end

    PD -.->|"父PTE 仍映射旧页"| PA_OLD
    CD ==>|"cow_handle_write<br/>set_pte 重新映射"| PA_NEW
```

**至此，父子进程在该虚拟地址上的物理内存已经彻底分离，互不干扰。**

---

## 附录：`do_fork` 全过程内存变化一览表

| 步骤 | 调用函数 (代码位置) | VA 空间变化 | PA 空间变化 | 页表变化 |
|:---:|:---|:---|:---|:---|
| 1 | `user_create_force()` (user.c:390) | 内核 VA 新增 child 指针 | 新增 struct user (内核堆PA) | 无 |
| 2 | `user_update_userid()` (user.c:394) | 无 | 修改 userid_bitmap 的 1bit | 无 |
| 3 | `mm_alloc(PAGE_SIZE)` (user.c:396) | 内核 VA 新增 child_pgdp | 新增 4KB 顶级页表 (内核堆PA) | 子 PGD 诞生(空白) |
| 4 | `memcpy(child_pgdp, default_pgd)` (user.c:400) | 无 | 子 PGD 被写入 512 个表项 | 子 PGD 高位项 = 内核映射 |
| 5 | `copy_level()` (cow.c:34) | 无 | 新增 N 个 4KB 中间级页表 | 子拥有独立 PMD/PTE 页表树 |
| 6 | `copy_leaf()` (cow.c:15) | 无 | 用户数据页 Ref: 1→2 | **父子PTE均: W→0, COW→1** |
| 7 | `local_flush_tlb_range()` (cow.c:110) | 无 | 无 | TLB 失效, 强制重新查表 |
| 8 | `cpu_context = parent->...` (user.c:420) | 无 | child 结构体内 ctx 字段被覆写 | 无 |
| 9 | `a0=0, sepc+=4` (user.c:421-423) | 无 | child 结构体内 a0/sepc 被篡改 | 无 |
| 10 | `继承元数据` (user.c:425-431) | 无 | child 的 code_va/pa/share 等字段被赋值 | 无 |
| 11 | `create_task()` (user.c:433) | 无 | 新增 task 结构体 + 内核栈 | `virt_to_phy(child_pgdp)` 记入 task |
| 后续 | `cow_handle_write()` (cow.c:114) | 无 | 新增 4KB 数据页, 旧页 Ref: 2→1 | 写入方 PTE: PFN改, W=1, COW=0 |
