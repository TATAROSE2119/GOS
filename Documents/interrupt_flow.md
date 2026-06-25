# GOS 中断处理流程

本文整理本工程中 GOS host、SBI、irqchip、timer、设备驱动以及 myGuest/virt 相关的中断处理路径。

## 一、整体分层

GOS 的中断处理可以按以下层次理解：

1. `mysbi/`：M-mode trap 入口，负责 trap delegation、SBI 调用；未启用 SSTC 时负责 CLINT M timer/software interrupt 转发。
2. `entry/`：S-mode trap 入口，保存/恢复上下文，并进入 C 语言异常/中断分发。
3. `core/irq/`：中断核心层，维护 `irq_domain`、逻辑 irq、hwirq 到 handler 的映射。
4. `drivers/irqchip/`：PLIC、IMSIC、APLIC 等具体中断控制器驱动。
5. `drivers/timer/` 与 `core/clock.c`：timer 中断、clock event、timer event 调度。
6. 设备驱动：调用 `get_irq()`/`msi_get_irq()` 和 `register_device_irq()` 注册自己的中断 handler。
7. `virt/` 与 `myGuest/`：虚拟化场景下的 VS timer、VS external interrupt、IMSIC interrupt file 映射与 guest 内部分发。

核心思想是：所有 S-mode 中断先以 RISC-V cause 号进入根 `INTC` domain，再由 `INTC` 上注册的 handler 分发到 timer、IPI、PLIC、IMSIC 等下一级中断源；外设最终在对应 irqchip domain 中按 hwirq 找到设备 handler。

本工程里的外部中断控制器不只有 PLIC，还包括 AIA 路径中的 APLIC 和 IMSIC，需要区分三种角色：

1. `PLIC`：传统线中断控制器。外设线中断进入 PLIC，PLIC 在 S external interrupt 中 claim 出 hwirq，再分发到设备 handler。
2. `IMSIC`：AIA MSI 中断控制器。设备或 APLIC 写 MSI 到 IMSIC interrupt file 后，IMSIC 触发 S external interrupt，GOS 通过 `CSR_TOPEI` 取出 MSI ID，再分发到设备 handler。
3. `APLIC`：本工程主要使用 MSI mode。APLIC 接收外设线中断，但不在 trap 路径里 claim；它把线中断转换成 MSI，写到 IMSIC，最终由 IMSIC 完成中断分发。

因此外部中断主路径有两类：

```text
线中断路径一：
外设线中断 -> PLIC -> INTC external cause 9 -> plic_handle_irq() -> 设备 handler

线中断路径二，AIA MSI mode：
外设线中断 -> APLIC(MSI mode) -> MSI 写入 IMSIC -> INTC external cause 9 -> imsic_handle_irq() -> 设备 handler

MSI 路径：
设备直接写 MSI -> IMSIC -> INTC external cause 9 -> imsic_handle_irq() -> 设备 handler
```

## 二、启动期初始化顺序

主启动路径在 `entry/main.c:start_gos()`：

```text
trap_init()
mm_init()
paging_init()
percpu_init()
set_online_cpumask(0)
irq_init()
init_timer(hw)
irqchip_setup(hw)
device_driver_init(hw)
bringup_secondary_cpus(hw)
percpu_tasks_init(0)
ipi_percpu_init()
vcpu_init()
user_init()
create_task("shell_init", ...)
enable_local_irq()
schedule()
```

关键点：

- `trap_init()` 设置 `stvec = do_exception_vector`，并写 `sie = -1` 打开各类 S-mode 中断使能位。
- `irq_init()` 创建根 irq domain：`INTC`。
- `init_timer(hw)` 根据设备表匹配 timer driver，目前主要是 `clint` driver；该 driver 同时支持传统 SBI/CLINT M timer 路径和 SSTC `stimecmp` 直接 S-mode timer 路径，并把 timer cause 注册到 `INTC`。
- `irqchip_setup(hw)` 根据设备表匹配 irqchip driver，例如 `PLIC`、`IMSIC`、`APLIC_S`、`APLIC_M`。
- `device_driver_init(hw)` 创建普通设备并 probe 设备驱动，设备驱动在 probe 中注册中断 handler。
- `enable_local_irq()` 最后设置 `sstatus.SIE`，中断才真正全局打开。

相关文件：

- `entry/main.c`
- `entry/trap.c`
- `core/irq/irq.c`
- `core/timer/timer.c`
- `core/device_driver.c`
- `include/asm/asm-irq.h`

## 三、S-mode trap 入口

S-mode trap 入口在 `entry/entry.S:do_exception_vector`。

硬件 trap 到来后：

1. 根据 `stvec` 进入 `do_exception_vector`。
2. 在当前栈上分配 `struct pt_regs` 空间。
3. 保存通用寄存器、浮点寄存器、可选 vector 状态。
4. 保存关键 CSR：
   - `sstatus`
   - `sepc`
   - `stval`
   - `scause`
   - `sscratch`
   - `satp`
5. 调用 `do_exception(regs, scause)`。
6. 返回后恢复 CSR、寄存器、栈指针。
7. 执行 `sret` 返回中断前上下文。

C 入口在 `entry/trap.c:do_exception()`：

```c
if (scause & (1UL << 63)) {
    task_scheduler_enter(regs);
    handle_irq(scause);
    task_scheduler_exit(regs);
} else {
    if (scause == EXC_BREAKPOINT)
        do_breakpoint(regs);
    else if (handle_exception(regs, scause))
        try_to_kill_task(regs);
}
```

也就是说：

- `scause` 最高位为 1：中断，进入 `handle_irq()`。
- `scause` 最高位为 0：同步异常，进入 page fault、illegal instruction、breakpoint 等异常处理。

## 四、irq core 数据结构与分发

irq core 主要文件：

- `core/irq/irq.c`
- `core/include/irq.h`

### 1. irq_domain

每个中断控制器会注册一个 `struct irq_domain`：

```c
struct irq_domain {
    char name[128];
    struct list_head irq_info_head;
    struct irq_domain *parent_domain;
    struct irq_domain *link_domain;
    struct irq_domain_ops *domain_ops;
    void *priv;
};
```

常见 domain：

- `INTC`：根 domain，对应 RISC-V S-mode cause。
- `PLIC`：传统线中断控制器，运行时通过 claim/complete 消费外设线中断。
- `IMSIC`：AIA MSI 中断控制器，运行时通过 `CSR_TOPEI` 消费 MSI interrupt identity。
- `APLIC_S`/`APLIC_M`：AIA APLIC，本工程主要使用 MSI mode，将输入线中断转换成 MSI 并投递到 IMSIC。

### 2. irq_info

`irq_info` 是逻辑 irq 的描述：

```c
struct irq_info {
    int irq;
    int in_used;
    void *priv;
    void (*handler)(void *data);
    void (*write_msi_msg)(...);
    void (*set_msi_desc)(...);
};
```

`domain_irq_info` 建立某个 domain 中 `hwirq -> irq_info` 的映射。

### 3. 根分发 handle_irq

`handle_irq(unsigned long cause)`：

1. 查找 `INTC` domain。
2. 从 `cause & ~SCAUSE_IRQ` 得到 RISC-V interrupt cause。
3. 在 `INTC` domain 中查找对应 `irq_info`。
4. 调用 `irq_info->handler(irq_info->priv)`。

因此 timer、IPI、external interrupt 都必须先在 `INTC` 上注册对应 cause 的 handler。

### 4. 下级分发 domain_handle_irq

`domain_handle_irq(domain, hwirq, data)`：

1. 在指定 irq domain 中查找 `hwirq`。
2. 找到对应 `irq_info`。
3. 调用最终 handler。

PLIC 和 IMSIC claim/读取出具体 hwirq 后，都会调用这个接口继续分发。

### 5. PLIC 与 AIA 外部中断拓扑

PLIC 和 AIA 都最终表现为 RISC-V S external interrupt，cause 号都是 `INTERRUPT_CAUSE_EXTERNAL = 9`，但下一级处理方式不同：

```text
PLIC:
INTC(cause 9) -> plic_handle_irq()
              -> read claim 得到 PLIC hwirq
              -> domain_handle_irq(PLIC domain, PLIC hwirq)

AIA MSI:
INTC(cause 9) -> imsic_handle_irq()
              -> read CSR_TOPEI 得到 IMSIC MSI ID
              -> domain_handle_irq(IMSIC domain, MSI ID)
```

APLIC 在 MSI mode 下不是 trap 入口 handler。它在初始化和 irq activate 阶段配置 source、target，把外设线中断转成 MSI；真正中断到达 CPU 后，由 IMSIC handler 处理。

## 五、Timer 中断流程

Timer driver 在 `drivers/timer/clint.c`。文件名叫 `clint`，但实现上不只支持传统 CLINT M-mode timer 转发，也支持 SSTC。是否经过 `mysbi` 取决于配置：

- `CONFIG_ENABLE_SSTC=y`：S-mode 直接写 `CSR_STIMECMP`，timer 到期后直接触发 S-mode timer interrupt，不需要 M-mode timer trap，也不需要 `mysbi` 设置 `mip.STIP`。
- `CONFIG_ENABLE_SSTC=n`：GOS 通过 `sbi_set_timer(next)` 请求 `mysbi` 写 CLINT `mtimecmp`，M timer 到期后由 `mysbi` 设置 `mip.STIP`，再进入 S-mode timer interrupt。
- `CONFIG_USE_RISCV_TIMER`：影响 clock source/counter 读取方式，可使用 `CSR_TIME`；未启用时从 CLINT `mtime` MMIO 读取。

初始化阶段：

1. `clint_timer_init()` 初始化 timer 设备；根据配置映射 CLINT，或允许 S-mode 读取 RISC-V `time` CSR。
2. `__timer_init()` 设置 `sie.STIE`。
3. 注册 clock source 和 per-cpu clock event。
4. 调用：

```c
domain_register_irq(NULL, d, INTERRUPT_CAUSE_TIMER,
                    timer_handle_irq, NULL);
```

这会把 `INTERRUPT_CAUSE_TIMER = 5` 注册到 `INTC` domain。

通用中断分发路径如下。无论 timer event 是由 SSTC 直接触发，还是由 `mysbi` 转发成 STIP，进入 GOS 后都走同一套 S-mode timer handler：

```text
timer 到期
  -> S-mode timer interrupt, scause = interrupt | 5
  -> do_exception_vector
  -> do_exception()
  -> handle_irq(cause=5)
  -> timer_handle_irq()
  -> do_clock_event_handler()
  -> event->evt_handler(event)
  -> clock_event_do_timer_list()
  -> 执行到期 timer_event handler
  -> program_next_event()
```

`timer_set_next_event()` 编程下一次事件时分两种路径：

### 1. SSTC 直接 S-mode timer 路径

启用 `CONFIG_ENABLE_SSTC` 时：

```text
program_next_event()
  -> timer_set_next_event(next)
  -> write_csr(CSR_STIMECMP, next)
  -> 硬件在 time >= stimecmp 时直接置起 S timer interrupt
  -> do_exception_vector
  -> handle_irq(cause=5)
  -> timer_handle_irq()
```

这条路径中，下一次 timer 事件完全由 S-mode 的 `stimecmp` 控制，不经过 SBI ecall，也不进入 M-mode timer interrupt。它依赖硬件实现 SSTC 扩展，并且 S-mode 有权限访问 `stimecmp`。

### 2. 传统 SBI/CLINT M timer 转发路径

未启用 `CONFIG_ENABLE_SSTC` 时：

```text
program_next_event()
  -> timer_set_next_event(next)
  -> sbi_set_timer(next)
  -> mysbi: SBI_SET_TIMER
  -> clint_timer_event_start()
  -> 写 CLINT mtimecmp
  -> M timer interrupt
  -> mysbi clint_timer_process()
  -> 设置 mip.STIP
  -> 因 STIP delegated 到 S-mode，进入 S timer interrupt
  -> do_exception_vector
  -> handle_irq(cause=5)
  -> timer_handle_irq()
```

这条路径需要 `mysbi` 协助把 M timer 转换成 S timer。

`core/clock.c` 负责维护每 CPU 的 timer event list。到期事件会执行 `timer_event_info->handler(data)`；周期 timer 会刷新 `expiry_time`，一次性 timer 会删除并释放。

## 六、IPI/software interrupt 流程

CLINT IPI 受 `CONFIG_SELECT_CLINT_IPI` 控制，代码也在 `drivers/timer/clint.c`。

初始化时：

```c
domain_register_irq(NULL, INTC, INTERRUPT_CAUSE_SOFTWARE,
                    clint_ipi_handler, NULL);
register_ipi(&clint_ipi_ops);
```

运行路径：

```text
send_ipi()
  -> clint_send_ipi()
  -> 写 CLINT msip
  -> S-mode software interrupt, scause = interrupt | 1
  -> handle_irq(cause=1)
  -> clint_ipi_handler()
  -> process_ipi(cpu)
  -> 清 sip.SSIP
```

AIA 配置下也可以使用 IMSIC IPI。IMSIC IPI 通过写目标 CPU interrupt file 触发 MSI ID，然后在 IMSIC domain 中注册 ID 1 的 handler。

## 七、PLIC 线中断流程

PLIC driver 在 `drivers/irqchip/plic/plic.c`。

初始化阶段：

1. `plic_init()` 映射 PLIC MMIO。
2. `__plic_init()` 初始化当前 CPU context：
   - 设置 threshold 为 0。
   - 关闭所有 hwirq enable。
   - 设置 `sie.SEIE`。
3. 初始化所有 priority 为 0。
4. 调用：

```c
irq_domain_init_cascade(&plic.irq_domain, name, &plic_irq_domain_ops,
                        parent, INTERRUPT_CAUSE_EXTERNAL,
                        plic_handle_irq, NULL);
```

这表示 PLIC 级联在 parent domain 上。通常 parent 是 `INTC`，hwirq 是 `INTERRUPT_CAUSE_EXTERNAL = 9`。

运行路径：

```text
外设拉线
  -> PLIC pending
  -> S-mode external interrupt, scause = interrupt | 9
  -> handle_irq(cause=9)
  -> plic_handle_irq()
  -> 读取 claim/complete 寄存器得到 hwirq
  -> domain_handle_irq(&plic.irq_domain, hwirq)
  -> 设备 handler
  -> 写 claim/complete 完成中断
```

`plic_handle_irq()` 在处理期间临时清 `sie.SEIE`，循环 claim 所有 pending hwirq，处理完再重新打开 `sie.SEIE`。

PLIC 的 mask/unmask 通过设置 priority 实现：

- mask：priority = 0
- unmask：priority = 1

affinity 通过每 CPU enable bitmap 控制：先关闭所有 CPU 上该 hwirq，再只在目标 CPU enable。

## 八、IMSIC MSI 中断流程

IMSIC driver 在 `drivers/irqchip/aia/imsic/imsic.c`。

IMSIC 是 AIA 中处理 MSI 的核心模块，既处理设备直接发出的 MSI，也处理 APLIC MSI mode 转换出来的 MSI。运行时 IMSIC 负责从 `CSR_TOPEI` 中取出 interrupt identity，然后在 `IMSIC` irq domain 中找到设备 handler。

初始化阶段：

1. `imsic_init()` 映射 IMSIC MMIO。
2. `imsic_state_setup()` 根据 BSP 私有数据初始化：
   - hart 数
   - guest 数
   - MSI ID 数
   - interrupt file base
   - used/enable bitmap
3. 调用：

```c
irq_domain_init_cascade(&imsic.irq_domain, name,
                        &imsic_irq_domain_ops, parent,
                        INTERRUPT_CAUSE_EXTERNAL,
                        imsic_handle_irq, &imsic);
```

也就是说 IMSIC 同样挂在 `INTC` 的 external cause=9 上。

运行路径：

```text
设备写 MSI addr/data
  -> 写入目标 CPU 的 IMSIC interrupt file
  -> S-mode external interrupt, scause = interrupt | 9
  -> handle_irq(cause=9)
  -> imsic_handle_irq()
  -> 读 CSR_TOPEI 得到 MSI ID
  -> domain_handle_irq(&imsic.irq_domain, id)
  -> 设备 handler
```

APLIC MSI mode 下的线中断最终也会走同一段 IMSIC 运行路径，只是 MSI 的来源不是 PCI/设备直接写 MSI，而是 APLIC 根据 target 配置生成 MSI。

IMSIC domain ops 负责：

- `alloc_irqs()`：分配 MSI ID。
- `get_msi_msg()`：根据 ID 的目标 CPU 生成 `msi_addr/msi_data`。
- `mask_irq()`/`unmask_irq()`：关闭/打开 IMSIC ID，PCI 设备还会同步 mask/unmask PCI MSI。
- `set_affinity()`：更新 MSI ID 的目标 CPU。

## 九、APLIC MSI 模式流程

APLIC driver 在：

- `drivers/irqchip/aia/aplic/aplic.c`
- `drivers/irqchip/aia/aplic/aplic_msi.c`

当前实现中 direct mode 标了 TODO，主要支持 MSI mode。APLIC 在这个模式下负责把线中断转换成 MSI，再交给 IMSIC 处理。

初始化阶段：

1. `aplic_init()` 读取 BSP 中的 APLIC 私有配置。
2. 如果是 M-mode APLIC，初始化 `SMSICFGADDR/SMSICFGADDRH` 指向 IMSIC。
3. 如果 `delegate` 为真，则配置 source delegation 后返回。
4. 如果 `mode == APLIC_MSI_MODE`，调用 `aplic_msi_setup()`。
5. `aplic_msi_setup()` 以 IMSIC domain 为 `link_domain` 创建 hierarchy domain。

APLIC MSI 模式中的关键点：

```text
外设线中断
  -> APLIC source
  -> APLIC 根据 target 配置生成 MSI addr/data
  -> MSI 写入 IMSIC interrupt file
  -> IMSIC 触发 S external interrupt
  -> imsic_handle_irq()
  -> CSR_TOPEI 得到 IMSIC MSI ID
  -> domain_handle_irq(&imsic.irq_domain, MSI ID)
  -> 设备 handler
```

在 `aplic_msi_activate_irq()` 中，会通过 link domain 查询 IMSIC 的 `msi_addr/msi_data`，然后写 APLIC target 寄存器，把 APLIC hwirq 绑定到对应 IMSIC MSI ID。

APLIC MSI mode 的注册/激活路径如下：

```text
设备 irq_parent = APLIC_S/APLIC_M
  -> get_irq(dev, irqs)
  -> alloc_irqs(dev, APLIC domain, irq_info, APLIC hwirq)
  -> APLIC domain 有 link_domain = IMSIC domain
  -> 先在 IMSIC domain 分配 MSI ID，并建立 MSI ID -> irq_info 映射
  -> 再在 APLIC domain 建立 APLIC hwirq -> 同一个 irq_info 映射
  -> domain_activate_irq(APLIC domain, APLIC hwirq)
  -> aplic_msi_activate_irq()
  -> 查询 link_domain 中对应 IMSIC MSI addr/data
  -> 写 APLIC target 寄存器
  -> unmask APLIC source
```

这样设计的结果是：设备驱动看到的是自己的逻辑 irq；APLIC domain 保存线中断 hwirq 到 `irq_info` 的关系；IMSIC domain 保存 MSI ID 到同一个 `irq_info` 的关系。运行时中断实际从 IMSIC 进入，所以最终通过 IMSIC MSI ID 找到同一个设备 handler。

## 十、设备驱动如何注册中断

设备信息来自 BSP 中的 `device_init_entry`：

```c
struct device_init_entry {
    char compatible[128];
    unsigned long start;
    unsigned int len;
    char irq_parent[128];
    int irq[MAX_IRQ_NUM];
    int irq_num;
    char iommu[128];
    int dev_id;
    void *data;
};
```

`core/device_driver.c:create_device()` 会：

1. 创建 `struct device`。
2. 拷贝 base/len/irq 数组。
3. 通过 `find_irq_domain(entry->irq_parent)` 找到设备所属 irq domain。
4. probe 匹配到的设备驱动。

### 1. 线中断设备

典型例子：`drivers/uart/ns16550a.c`

```c
nr_irqs = get_irq(dev, irqs);
for (i = 0; i < nr_irqs; i++)
    register_device_irq(dev, irqs[i], ns16550a_irq_handler, NULL);
```

`get_irq()` 做的事：

1. 分配逻辑 irq。
2. 根据设备的 `dev->irq_domain` 和 BSP 中的 `dev->irqs[i]` 建立 hwirq 映射。
3. 调用 domain ops 做 activate、set_type、unmask、set_affinity。

如果 `irq_parent` 是 `PLIC`，运行时由 PLIC claim 得到这个线中断 hwirq。

如果 `irq_parent` 是 `APLIC_S`/`APLIC_M` 且 APLIC 为 MSI mode，`get_irq()` 仍然从设备角度处理线中断 hwirq，但激活时会额外通过 `link_domain = IMSIC` 分配一个 MSI ID，并把 APLIC target 配成投递到该 IMSIC ID。运行时 CPU 收到的是 IMSIC 的 MSI 中断，而不是 APLIC 自己的 trap handler。

### 2. MSI/MSI-X 设备

PCI MSI/MSI-X 典型路径在 `drivers/pci/msi.c`：

```text
pci_msix_enable()
  -> pci_msix_get_vec_count()
  -> pci_msix_map()
  -> msi_get_irq()
  -> 写 MSI-X table addr/data
  -> enable MSI-X capability
```

`msi_get_irq()` 做的事：

1. 为每个 vector 分配逻辑 irq。
2. 调用 domain ops 的 `alloc_irqs()` 分配 MSI hwirq。
3. 通过 `get_msi_msg()` 获取 MSI addr/data。
4. 调用设备提供的 `write_msi_msg()` 写入设备 MSI/MSI-X 配置。

设备驱动随后调用 `register_device_irq()` 注册自己的 handler。

## 十一、M-mode/SBI 与中断 delegation

SBI 代码在 `mysbi/`。

M-mode trap 入口：

- `mysbi/sbi/sbi_entry.S:exception_vector`
- `mysbi/sbi/sbi.c:sbi_trap_handler()`

`sbi_init()` 会：

1. 设置 `mtvec = exception_vector`。
2. 初始化 PMP。
3. 初始化 CLINT。
4. 可选初始化 M-mode IMSIC。
5. 设置 `mie = MIP_MSIP | MIP_MEIP | MIP_MTIP`。
6. 调用 `delegate_traps()`。

`delegate_traps()` 把以下中断委托到 S-mode：

```c
interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;
write_csr(mideleg, interrupts);
```

这意味着 S software、S timer、S external 会直接进 GOS 的 S-mode trap。

未启用 SSTC 时，CLINT timer 需要 M-mode 协助：

- GOS 调 `sbi_set_timer(next)`。
- SBI 中 `SBI_SET_TIMER` 调 `clint_timer_event_start()` 写 CLINT mtimecmp。
- M timer 到期进 M-mode。
- `clint_timer_process()` 清 `mie.MTIP` 并设置 `mip.STIP`。
- 因 STIP 被 delegated，随后进入 S-mode timer interrupt。

启用 SSTC 时，GOS 的 `timer_set_next_event()` 直接写 `CSR_STIMECMP`，timer 到期直接产生 S-mode timer interrupt。这条路径不需要 `SBI_SET_TIMER`，也不依赖 `mysbi` 的 `clint_timer_process()` 转发。

CLINT software interrupt 类似：

- M-mode 清 CLINT msip。
- 设置 `mip.SSIP`。
- S-mode software interrupt 进入 GOS。

## 十二、虚拟化中断流程

virt 相关文件：

- `virt/virt.c`
- `virt/vcpu_timer.c`
- `virt/vcpu_aia.c`
- `virt/imsic_emulator.c`
- `myGuest/entry/trap.c`
- `myGuest/core/irq.c`
- `myGuest/drivers/imsic.c`
- `myGuest/drivers/clint.c`

### 1. Guest trap 初始化

myGuest 启动时：

```text
trap_init()
mm_init()
paging_init()
create_devices()
__enable_local_irq()
```

guest 的 `trap_init()` 同样设置 `stvec = do_exception_vector`，并写 `sie = -1`。

guest `do_exception()` 只做较简单的分发：

```c
if (interrupt) {
    if (cause == INTERRUPT_CAUSE_TIMER)
        irq_do_timer_handler();
    else if (cause == INTERRUPT_CAUSE_EXTERNAL)
        irq_handler();
}
```

### 2. VS timer 注入

host 侧 `vcpu_timer.c`：

1. guest 设置下一次 timer 时，host 通过 `vcpu_timer_next_event()` 注册一个 host timer。
2. host timer 到期后执行 `vcpu_timer_handler()`。
3. `vcpu_timer_handler()` 调用 `vcpu_set_interrupt(vcpu, IRQ_VS_TIMER)`。
4. vCPU 进入前，`vcpu_do_interrupt()` 检查 pending 位并写 `vcpu->cpu_ctx.hvip`。
5. `vcpu_restore()` 后硬件看到 `HVIP.VSTIP`，向 VS 模式注入 timer interrupt。
6. myGuest 进入 `irq_do_timer_handler()`。

如果启用 SSTC/VS SSTC，则流程会改为写 `vstimecmp`/`stimecmp` 相关 CSR。

### 3. VS external/AIA 直通

AIA 虚拟化主要在 `virt/vcpu_aia.c`：

1. 为 vCPU 分配 HGEI。
2. 查 IMSIC 中该 CPU、该 guest index 对应的 interrupt file。
3. 将 interrupt file HPA 映射到 guest GPA。
4. 设置 guest `hstatus.VGEIN`。
5. 设备 MSI 直接写 guest VS interrupt file。
6. guest 收到 VS external interrupt。
7. myGuest `irq_handler()` 调用 guest IMSIC handler。
8. guest IMSIC 读 `CSR_TOPEI` 得到 ID，再调用 `do_device_irq_handler(hwirq)`。

myGuest 内部的 MSI 注册流程：

```text
alloc_msi_irqs()
  -> guest imsic alloc ID
  -> unmask ID
compose_msi_msg()
  -> 返回 guest interrupt file addr/data
register_irq_handler()
  -> 建立 hwirq 到 guest handler 的映射
```

## 十三、调试路径建议

调试中断问题时可以按以下顺序查：

1. 是否进 trap：检查 `stvec`、`sstatus.SIE`、`sie` 对应位。
2. 是否进入正确 cause：打印 `scause & ~SCAUSE_IRQ`。
3. `INTC` domain 是否注册了对应 cause handler：
   - timer cause 5
   - software cause 1
   - external cause 9
   - guest external cause 12
4. irqchip 是否 claim/读到了具体 hwirq：
   - PLIC claim register
   - IMSIC `CSR_TOPEI`
5. 具体 domain 中是否存在 hwirq 映射：
   - `get_irq_info(domain, hwirq)`
6. 设备 handler 是否注册：
   - `register_device_irq()`
   - guest 中是 `register_irq_handler()`
7. APLIC MSI mode 场景确认：
   - APLIC sourcecfg 是否配置为正确触发类型。
   - APLIC target 是否写入了对应 IMSIC hart/guest/EIID。
   - APLIC source 是否 unmask。
   - IMSIC 中对应 MSI ID 是否 enable。
8. MSI 场景确认 MSI addr/data 是否写入设备或 APLIC target。
9. affinity 场景确认目标 CPU 是否 online，PLIC enable 或 IMSIC target 是否更新。
