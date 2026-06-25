# Gos fork + COW 机制 —— 任务书（实施分解）

> 版本：v0.1（草案）　日期：2026-06-25　配套：《COW_fork_需求书.md》

本任务书将需求落到模块、文件、接口与里程碑。所有引用位置基于当前代码梳理，实施前需再核对源。

## 一、总体技术路线

```
┌── fork(sys_fork) ──► copy_page_range(父→子: 共享+只读+COW, get_page)
│                         │
任务/地址空间层            ▼
core/task, user      子任务独立 satp/ASID, 共享父物理页(只读)
                          │
                    ── 写入只读共享页 ──► Store/AMO Page Fault (scause=15)
异常层 entry/trap          │
                          ▼
缺页层 mm/pgtable    do_page_fault(addr, cause):
                       若 COW 写保护:
                         refcount==1 → 就地恢复写权限
                         refcount> 1 → 新页+拷贝+换PTE+put_page+精确刷TLB
物理页层 mm/mm        refcount[pfn]: get_page/put_page，归零才真正 free
```

## 二、工作分解（WBS）

### 模块 0：可裁剪开关（Kconfig）
- **O-1** 在 `mm/Kconfig`（及/或 `core/task/Kconfig`）新增 `CONFIG_COW`、`CONFIG_FORK`，默认关闭；`include/mm`、`autoconf` 联动。
- 产出：配置项；关闭时不编译新增代码、不改变既有行为（对应 NFR-3 / AC-4）。

### 模块 1：物理页引用计数（FR-1，地基，最高优先级）
- **文件**：`mm/mm.c`、`include/gos/mm.h`
- **M1-1** 在物理内存初始化（`paging_init` 路径）时，按物理内存范围建立 `unsigned short *page_refcount`（按 PFN 索引；或 `struct page` 数组，含 refcount 字段）。
- **M1-2** 接口：
  - `void get_page(unsigned long pa);`　// refcount++（原子）
  - `int  put_page(unsigned long pa);`　// refcount--，归零返回 1 并真正释放
  - `int  page_count(unsigned long pa);`
- **M1-3** 改造释放路径：`mm_free()`/`__mm_free()`（`mm/mm.c:476/424`）在 COW 页上经 `put_page` 决策是否真正清 bitmap，避免误回收共享页。
- **依赖**：无。**风险**：R-2（并发）。

### 模块 2：PTE COW 原语（FR-2）
- **文件**：`include/asm/pgtable.h`、`mm/pgtable.c`
- **M2-1** 定义 `#define _PAGE_COW _PAGE_SOFT`（bit 8，`include/asm/pgtable.h:113` 区域）。
- **M2-2** 原语（参照现有 `pfn_pte`/`pte_pfn`/`pte_is_valid`）：
  - `pte_t pte_wrprotect(pte_t)`　// 清 `_PAGE_WRITE`
  - `pte_t pte_mkcow(pte_t)` / `pte_t pte_uncow_mkwrite(pte_t)`
  - `int   pte_is_cow(pte_t)`
  - `void  set_pte(unsigned long *ptep, pte_t)`　// 统一写入点
- **依赖**：无。

### 模块 3：地址空间复制（FR-3）
- **文件**：新增 `mm/cow.c`（或并入 `mm/pgtable.c`）、`user/`
- **M3-1** `int copy_page_range(void *dst_pgdp, void *src_pgdp, range)`：遍历父用户区叶子 PTE（复用 `mmu_pt_walk_fetch`/`riscv_pt_walk_alloc`，`mm/pgtable.c:41/117`）：
  - 对可写匿名页：父子两侧 `pte_wrprotect + pte_mkcow`，子侧建同 PFN 映射，`get_page(pfn)`；
  - 共享内存区 / 设备映射：按需直接共享或跳过（见需求书 §六）。
- **M3-2** 复制后对父地址空间相应范围刷 TLB（`local_flush_tlb_range`，`mm/pgtable.c:329`）。
- **依赖**：M1、M2。

### 模块 4：fork 接口与系统调用（FR-4）
- **文件**：`core/task/task.c`、`user/syscall.c`、`user/user_exception.c`、`include/uapi/syscall.h`、`myUser/core/syscall.S`
- **M4-1** 内核侧 `int do_fork(struct task *parent)`：分配子 PGD（参照 `create_user_task`，`core/task/task.c:222`，现状仅 `memcpy` 顶层 PGD，`task.c:236`）→ 调 `copy_page_range` 深拷用户区 → 复制父 `pt_regs`、置子返回值寄存器为 0 → 分配新 ASID → 入调度。
- **M4-2** 注册 `sys_fork` 到 `syscall_table`（`user/syscall.c:71`），新增 `__NR_fork`（`include/uapi/syscall.h`）；U 态封装 `fork()`（`myUser/lib`/`core`）。
- **M4-3** 父子返回值约定：父=子 id，子=0。
- **依赖**：M3。

### 模块 5：COW 缺页处理（FR-5、FR-6，核心路径）
- **文件**：`entry/trap.c`、`mm/pgtable.c`
- **M5-1** 故障原因传递（FR-6）：`handle_exception`（`entry/trap.c:176`）将 `scause`（区分 12/13/15）与特权级传入 `do_page_fault(addr, cause, from_user)`，改其签名（`mm/pgtable.c:672`）。
- **M5-2** 在 `do_page_fault` 新增「写保护缺页」分支（注意现状第 682 行「PTE 有效即 return -1」需为 COW 让路）：
  - 取叶子 PTE；若 `pte_is_valid && !writable && pte_is_cow && cause==STORE`：
    - `page_count(pfn)==1` → `set_pte(pte_uncow_mkwrite(pte))`，刷该页 TLB，返回 0（无需拷贝，优化）；
    - `>1` → `new = mm_alloc(4K)`；拷贝旧页内容；`set_pte` 指向新页且可写、清 COW；`put_page(old_pfn)`；`local_flush_tlb_page_asid()`（`include/asm/tlbflush.h`）；返回 0。
  - 其余情况维持原 VMAP 惰性分配逻辑（不回归，R-1）。
- **M5-3** 多 hart：同页并发缺页用 per-page 锁或对 PTE 做原子 CAS，确保只拷一次。
- **依赖**：M1、M2。

### 模块 6：测试 command（FR-7、NFR-5）
- **文件**：`app/command/cow_test.c`（核心态）、`myUser/command/fork_cow_test.c`（U 态），并登记各自 `Makefile` 与 `APP_COMMAND_REGISTER`（参考 `Documents/command.md`、`myUser/core/command.c:102`）。
- **T-1** 基本隔离：`fork` 后父子写同一逻辑地址，互不可见（→ AC-1）。
- **T-2** 计数自检：统计写保护缺页次数与拷贝次数，校验「共享页首写触发一次拷贝、独占页写不拷贝」（→ AC-2）。
- **T-3** 多 hart 竞争：多核同时写同一 COW 页，校验结果自洽、无串扰（→ AC-3）。
- **T-4**（可选）超页/ASID/`sinval` 组合场景，复用 `page_tlb_test.c`、`sinval_test.c` 思路。

## 三、接口清单（交付物）

| 层 | 新增/修改接口 | 文件 |
|---|---|---|
| 物理页 | `get_page/put_page/page_count` | `mm/mm.c`,`include/gos/mm.h` |
| PTE | `pte_wrprotect/pte_mkcow/pte_is_cow/pte_uncow_mkwrite/set_pte`、`_PAGE_COW` | `include/asm/pgtable.h` |
| 地址空间 | `copy_page_range` | `mm/cow.c` |
| 任务 | `do_fork` | `core/task/task.c` |
| 系统调用 | `sys_fork`、`__NR_fork`、U 态 `fork()` | `user/syscall.c`,`include/uapi/syscall.h`,`myUser/` |
| 缺页 | `do_page_fault(addr,cause,from_user)`（改签名）+ COW 分支 | `entry/trap.c`,`mm/pgtable.c` |
| 测试 | `cow_test`、`fork_cow_test` command | `app/command/`,`myUser/command/` |

## 四、里程碑与排期建议

| 里程碑 | 内容 | 依赖 | 退出标准 |
|---|---|---|---|
| **MS-1 地基** | 模块 0+1+2（refcount + PTE 原语 + Kconfig） | — | 单元自测：refcount 增减/归零释放正确；PTE 原语位操作正确 |
| **MS-2 派生** | 模块 3+4（copy_page_range + fork） | MS-1 | fork 出子任务、子地址空间为只读共享、可调度运行 |
| **MS-3 写时复制** | 模块 5（COW 缺页闭环） | MS-1,MS-2 | 单核下 AC-1/AC-2 通过 |
| **MS-4 多核与测试** | 模块 5 并发 + 模块 6 | MS-3 | AC-3 通过；QEMU + 一种目标平台 pass/fail 明确（AC-5） |
| **MS-5 回归与文档** | AC-4 回归 + 新增 `Documents/cow.md` | MS-4 | 关闭 CONFIG 时既有测试与编译不受影响 |

> 建议关键路径：MS-1 → MS-3，因为 refcount 与缺页闭环是技术风险集中点（R-1/R-2），应尽早打通最小可用链路再扩展并发与超页。

## 五、验证与平台

- 构建：沿用 `./build.sh default`（QEMU 全量）→ `./build.sh run`；目标平台用 `fpga-h`/`vcs-h` 等配置（README §二）。
- 调试：`make run-debug`（`-S -s`）配合缺页打印（NFR-1）。
- 每个 command 须输出明确 `PASS/FAIL` 及关键计数（缺页次数、拷贝次数、refcount 终值），适配无人值守（NFR-5）。

## 六、待评审决策点（开工前需确认）

1. fork 语义裁剪程度（轻量派生 vs 接近 Unix）——影响模块 4 工作量。
2. COW 是否纳入共享内存区 `user_share_memory_*`（`include/gos/user.h`）。
3. 引用计数数据结构（PFN 数组 vs `struct page`）——影响内存占用与后续可扩展性。
4. 本期是否支持超页 COW 拆分（否则 COW 区强制 4K 映射）。
5. 多 hart 缺页串行化策略（per-page 锁 vs PTE 原子 CAS）。

## 七、关联文档
《COW_fork_需求书.md》、`Documents/mm.md`、`Documents/task.md`、`Documents/user.md`、`Documents/command.md`、`Documents/interrupt_flow.md`。
