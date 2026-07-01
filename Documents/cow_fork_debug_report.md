# fork + COW 单核缺陷排查报告

> 项目：GOS（RISC-V 教学 OS）Copy-On-Write fork
> 场景：单核（`-smp 1`）下 `user_run fork_cow_test` 挂死 / COW 隔离失败
> 结论：定位到 **3 个相互纠缠的根因**，全部修复后 `COW isolation: TEST PASS`（commit `82e0182`）

---

## 一、背景

`fork_cow_test` 是一个 U 态测试命令，逻辑如下：

```c
static volatile int cow_val = 100;          // 数据段全局变量

int handler() {
    print("before fork: cow_val=%d", cow_val);
    int pid = fork();
    if (pid == 0) {                          // 子
        cow_val = 200;
        print("[child] cow_val=%d", cow_val);   // 期望 200
        while (1) {}                         // 无 exit()，停住
    } else {                                 // 父
        for (volatile long i=0;i<2e7;i++);   // 略等子先写
        print("[parent] child_pid=%d cow_val=%d", pid, cow_val);  // 期望 pid≠0, 100
        print(cow_val==100 ? "TEST PASS":"TEST FAIL");
    }
}
```

**预期**：父子写同一逻辑地址互不可见（AC-1 隔离）。子写 `cow_val` 触发写时复制，父仍读到 `100`。

---

## 二、症状

单核下运行，输出停在第一行后**整机挂死**：

```
[fork_cow] before fork : cow_val=100
（无后续输出，直到 timeout）
```

多核（`-smp 4`）下更糟：SBI 层 `sbi panic`，非法指令 `mcause=0x2`。

> ⚠️ **一个极具迷惑性的现象**：在真正隔离修复之前，测试曾"跑出"过 `[child]`/`[parent]`，但结果是
> `child_pid=0 cow_val=200`（父也看到 200）。这是**假象**——父子共享地址空间、栈被互相踩，
> 导致 `pid` 变量被冲成 0、`cow_val` 被子改动可见。**排查初期务必警惕"能跑=对了"的错觉。**

---

## 三、如何复现

```bash
# 1) 写入自动运行命令并编译
printf 'user_run fork_cow_test\n' > auto_run.bin
make

# 2) 单核启动（默认 make run 是 -smp 4，现象不同，见第六节）
timeout 40 ./qemu-system-riscv64 -nographic \
  -machine virt,aia=aplic-imsic,aia-guests=7 -smp 1 \
  -cpu rv64,sv39=on,sv48=on,sv57=on,svnapot=on,svpbmt=on,svinval=on,zicond=on,v=on,Zfh=on,Zfhmin=on,smstateen=on \
  -m 8G -device my_dmaengine -device my_chr_display \
  -drive if=none,file=./init.img,id=nvm -device nvme,drive=nvm,serial=deadbeef \
  -bios out/Image.bin
```

> **注意**：`fork_cow_test` 是 U 态命令（`myUser/command/`），须用内核命令 `user_run <cmd>` 启动，
> 不能直接 `fork_cow_test`（内核 shell 找不到 U 态命令）。

---

## 四、调试过程（逐层剥开）

排查采用 **printf 埋点追踪 + GDB + 单核化简** 组合。关键在于每一层都用最小实验证伪/证实一个假设。

### 4.0 前置绊脚石：命令根本没被分发

最初连 `[fork_cow] before fork` 都没有，`do_command` 找不到命令。
排查发现 `myUser.elf` 的 link map 里**有**该命令（`.command_init_table` 段包含它），但运行时找不到。

- **根因**：内核通过 `entry/user_bin.S` 的 `.incbin "build/myUser.bin"` 把用户二进制嵌进镜像，
  而 **make 对 `.incbin` 的依赖追踪失效**——改了 `myUser/` 代码后 `user_bin.o` 不重新 incbin，
  镜像里是**旧用户程序**（不含新命令）。
- **修复**：`make clean && make`（强制刷新 incbin）。
- **教训**：改 `myUser/` 后必须清理重建，否则调的是旧程序。

### 4.1 子任务进 U 态即崩 → 缺页没被处理

命令能跑后，`before fork` 之后崩溃，dump 显示 `scause=0xf`（Store 缺页）、`sbadaddr=0x81f0c`（栈地址），
被当作**致命异常**杀掉。

在 `do_page_fault`（`mm/pgtable.c`）的 COW 分支埋点，发现**该分支根本没进**：

```
[COWDBG] store fault ...   ← 从未打印
```

- **根因①（缺页路由错）**：本 OS 的 **U 态异常走 `user/user_exception.c:do_user_exception`**，
  **不经过 `entry/trap.c:do_exception`**。而 COW 处理最初错误地加在了 `do_page_fault`（内核路径）。
  `do_user_exception` 的 `switch(scause)` 只有 SYSCALL/ILLEGAL_INST，缺页全落到 `default → 杀任务`。
- **修复**：在 `do_user_exception` 新增 `case EXC_STORE_PAGE_FAULT`，调 `cow_try_handle_store()`。

### 4.2 隔离失败：父也看到 200

缺页能处理后，输出 `TEST FAIL`：`[child] 200` / `[parent] child_pid=0 cow_val=200`。

在 `cow_handle_write` 埋点，3 次 COW 缺页 `count=2` 都走了拷贝路径（逻辑对），子确实把数据页
`0xc02bf000` 拷到 `0xc0300000` 并写 200。**但父从没碰过数据页，却读到 200** → 只能是父子页表项是同一块内存。

在 `copy_page_range` 打印顶层项，铁证：

```
copy_range dst_pgd[0]=0x30040401 src_pgd[0]=0x30040401   ← 完全相同
```

- **根因②（页表共享）**：`do_fork` 用 `memcpy(child_pgd, default_pgd)` 建子 PGD，
  把内核 PGD 的 **PGD[0]（用户区顶层项）也复制了**，指向父/内核**共享的下级用户页表**。
  `copy_page_range` 因 `dst_tbl[i]!=0` 直接写进共享表 → 改子 = 改父 → 毫无隔离。

### 4.3 第一次隔离修复反而挂死 → 清掉了内核映射

尝试"在 `do_fork` 里清 `child_pgd[0]` 强制建独立表"。结果**整机挂死**，多核下 SBI `csrr mstateen0` 非法指令 panic。

GDB 单步 + 页表 dump 发现：子的用户页表其实**建对了**（code/data 映射正确、COW 位对）。
但 GDB `break user_child_start` 不命中、`__switch_task` 打印显示**切到了 `user_child` 但子不执行**。

用 `nm build/gos.elf` 查内核符号 VA：

```
do_exception     @ 0x802007e2
task_fn_wrap     @ 0x80212d6a     ← 内核代码在低地址！
```

- **根因③（隔离破坏内核映射）**：**内核在低地址 `0x80200000+` 执行**，而 sv48 下 PGD[0] 覆盖 `[0, 512GB)`——
  **内核代码(0x80200000) 与用户区(0x1000) 同在 PGD[0]**。整表清 `child_pgd[0]` 把**内核代码映射一起清掉**了。
  子切到 `child_pgd` 后执行 `task_fn_wrap`（0x80212d6a）**取指失败** → 挂死 / 跳飞到 SBI。

### 4.4 子任务始终不被调度 → U 态不可抢占

修隔离的同时还发现：`[CHILD] user_child_start` 探针**一次都没打印**，子任务从未执行。
单核异常序列：

```
[EXC] scause=0x8          (fork 的 ecall)
[EXC] scause=0xf          (父栈 COW 缺页，已处理)
[EXC] scause=0x8...5      (定时器中断) → 之后全停
```

- **根因（附带，调度层）**：U 态定时器中断落到 `do_user_exception`，而它**对中断不做任何处理、不触发调度**。
  于是 U 态任务**不可抢占**，父独占 CPU，fork 出的子永远轮不到。
- **修复**：在 `do_user_exception` 中，定时器中断时调 `schedule()`（把当前用户任务移到队尾并立即重设定时器，
  返回用户循环 `enable_local_irq()` 时定时器打到内核 `do_exception` 完成任务切换）。

---

## 五、根本原因与最终修复

| # | 根本原因 | 修复位置 | 修复要点 |
|---|---|---|---|
| ① | U 态缺页走独立路径 `do_user_exception`，COW 加错在 `do_page_fault` | `user/user_exception.c` | 新增 `EXC_STORE_PAGE_FAULT` case 调 `cow_try_handle_store` |
| ② + ③ | 子 PGD 复制内核低地址映射；用户区与内核代码同在 PGD[0]，无法整表清 | `mm/cow.c: copy_level` | **深拷贝(克隆)**：遍历用户区时若子某级页表项与父指向同一下级表，克隆整表(保留内核兄弟项)，令子拥有私有下级表 |
| — | U 态任务不可抢占，子不被调度 | `user/user_exception.c` | 定时器中断调 `schedule()` |

### 关键修复：`copy_level` 深拷贝（`mm/cow.c`）

```c
} else {                      // 非叶中间项
    unsigned long *src_child = phy_to_virt(pfn_to_phys(pte_pfn(src_pte)));
    if (dst_tbl[i] == 0) {
        // 子无此级表：新建空表
        pa = alloc_zero_page(0);
        dst_tbl[i] = (pa>>PAGE_SHIFT)<<_PAGE_PFN_SHIFT | _PAGE_PRESENT;
    } else if (dst_tbl[i] == src_pte) {
        // 子该项与父指向同一张下级表(继承自内核 PGD)：
        // 克隆整表(复制以保留内核等兄弟项)，令子拥有私有下级表
        pa = alloc_zero_page(0);
        memcpy(phy_to_virt(pa), src_child, PAGE_SIZE);
        dst_tbl[i] = (pa>>PAGE_SHIFT)<<_PAGE_PFN_SHIFT | _PAGE_PRESENT;
    }
    // else: 子已有私有表(前一 region 克隆)，复用
    dst_child = phy_to_virt(pfn_to_phys(pte_pfn(dst_tbl[i])));
    copy_level(dst_child, src_child, va, shift-9, start, end);
}
```

**原理**：用户区(L3[0]) 与内核代码(L3[2]) 只在 PGD[0] 层共享。逐级遇到"子/父同表"就克隆，
到用户叶子层才改 PTE（COW），内核项(如 L3[2])因不在遍历范围内被原样保留 → **只隔离用户区、不碰内核映射**。

---

## 六、验证

```
[fork_cow] before fork : cow_val=100
[child]  cow_val=200 (expect 200)          ✓
[parent] child_pid=1 cow_val=100           ✓   隔离成立、pid 正确
COW isolation: TEST PASS                   ✓
```

- **单核（-smp 1）**：PASS ✅（MS-3 / AC-1 达成）
- **多核（-smp 4）**：仍挂死 ⏸ —— 独立的并发/跨核问题（MS-4，任务书 M5-3：per-page 锁 / PTE 原子 CAS / 跨核 ASID），不在本报告范围。

---

## 七、经验教训（架构陷阱，非代码可见）

1. **警惕"假象通过"**：共享地址空间会让 fork 测试"跑出结果"但全是乱码。隔离没做对之前的任何"通过"都不可信。
2. **内核在低地址执行**（0x80200000+），与用户 4GB 区同在 sv48 的 PGD[0]。fork 隔离**不能整表清 PGD[0]**，必须深拷贝。
3. **U 态异常独立路径**：走 `do_user_exception`，不经 `entry/trap.c`。缺页处理、抢占都要加在这里。
4. **U 态默认不可抢占**：需在 U 态定时器中断里主动 `schedule()`。
5. **`.incbin` 用户镜像不自动刷新**：改 `myUser/` 后必须 `make clean`。
6. **调试方法**：printf 埋点逐层证伪 → GDB 定位跳飞/符号 VA → **单核化简**排除多核干扰（关键一步，让静默挂死变得可追）。

---

*报告对应提交：`82e0182`（及前置 WIP `eb95ed4`）*
