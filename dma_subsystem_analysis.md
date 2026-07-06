# GOS DMA 子系统与测试全景流程图解

> 本文档严格参考 GOS 项目源码，包含 `app/command/dma_test.c`、`core/dmac/dmac.c`、`core/dma-mapping/dma-mapping.c`、`core/dma-mapping/iova.c`、`drivers/dmac/dmac_dw_axi.c`，
> 使用全景图还原 DMA 数据拷贝测试的完整软硬件生命周期：从应用层发起、核心层设备匹配、IOVA虚拟地址分配、硬件驱动层配置、一直到中断返回的全部细节。

---

## 核心流程全景图：DMA Test 执行完整调用链

```mermaid
flowchart TB
    %% ═══════════════════════════════════════════════
    %% 第一部分：应用程序层 (APP Layer)
    %% ═══════════════════════════════════════════════

    subgraph APP_LAYER ["🟦 应用程序层 — 发起 dma_test 命令"]
        direction TB
        APP1["📄 app/command/dma_test.c:31-85<br/><b>cmd_dma_test_handler（）</b><br/>═══════════════════════════<br/>① 解析参数：获取设备名 name 和 size<br/>② src = mm_alloc(size)<br/>   dst = mm_alloc(size)<br/>③ 初始化 src 内存（赋值为 0~size-1）<br/>④ start_time = get_system_time_ms()"]
        
        APP2["<b>调用硬件加速拷贝</b><br/>═══════════════════<br/>ret = <b>memcpy_hw</b>(name, dst, src, size);<br/>  <i>进入内核 DMA 核心子系统处理 ↓</i>"]
        
        APP3["<b>校验与统计</b><br/>═══════════════════<br/>① diff = get_system_time_ms() - start_time<br/>② 遍历比较 src 与 dst 内存内容是否一致<br/>③ 打印 dma_test success 及耗时"]
        
        APP1 --> APP2
        APP2 -.->|"返回结果"| APP3
    end

    %% ═══════════════════════════════════════════════
    %% 第二部分：DMA 核心层 (DMA Core Layer)
    %% ═══════════════════════════════════════════════

    subgraph DMA_CORE ["🟧 DMA 核心抽象层 (Core)"]
        direction TB
        DC1["📄 core/dmac/dmac.c:128-142<br/><b>memcpy_hw（name, dst, src, size）</b><br/>═══════════════════════════<br/>遍历全局 <b>dmacs</b> 链表（LIST_HEAD）<br/>匹配 dmac→name == 传入参数的 name<br/>找到后，调用 <b>dma_transfer</b>(dmac, dst, src, size, DMAC_XFER_M2M)"]
        
        DC2["📄 core/dmac/dmac.c:94-126<br/><b>dma_transfer（）</b><br/>═══════════════════════════<br/>① 参数校验（若启用 IOMMU 则检查是否 PAGE_SIZE 对齐）<br/>② 为 src 申请 DMA 映射：<br/>   <b>dma_mapping</b>(dmac→dev, virt_to_phy(src), &src_iova, size)<br/>③ 为 dst 申请 DMA 映射：<br/>   <b>dma_mapping</b>(dmac→dev, virt_to_phy(dst), &dst_iova, size)"]
        
        DC3["调用底层驱动方法：<br/>dmac→ops→<b>transfer_m2m</b>(src_iova, dst_iova, size, dmac→priv)"]
        
        DC1 --> DC2 --> DC3
    end

    %% ═══════════════════════════════════════════════
    %% 第三部分：DMA 映射与 IOVA 分配 (DMA Mapping)
    %% ═══════════════════════════════════════════════

    subgraph DMA_MAPPING ["🟪 DMA 映射子系统 & IOVA 分配"]
        direction TB
        DM1["📄 core/dma-mapping/dma-mapping.c:25-60<br/><b>dma_mapping（dev, addr, &ret_iova, len）</b><br/>═══════════════════════════════<br/>计算所需的页数 align_nr"]
        
        DM2{"是否启用 IOMMU?<br/>(!iommu || !iommu→ops→map_pages)"}
        
        DM3["<b>无 IOMMU (物理透传)</b><br/>═══════════════════<br/>*ret_iova = addr (直接返回物理地址)<br/>return 0;"]
        
        DM4["<b>启用 IOMMU</b><br/>═══════════════════<br/>获取 iommu_group = iommu_get_group(dev)<br/>调用 <b>iova_alloc</b> 申请虚拟地址空间"]
        
        DM5["📄 core/dma-mapping/iova.c:27-63<br/><b>iova_alloc（&group→iova_cookie, size）</b><br/>═══════════════════════════════<br/>First-Fit 算法寻找空闲虚拟地址：<br/>遍历 iovad 链表，寻找 `iova→base + iova→size + size < next→base` 的空隙<br/>如果有空隙，new→base = iova→base + iova→size<br/>如果没有，new→base = 0 (IOVA_START)<br/>加入链表，返回分配的 <b>iova</b>"]
        
        DM6["调用 <b>iommu→ops→map_pages</b><br/>建立 IOVA 到物理地址的 IOMMU 页表映射<br/>*ret_iova = iova"]
        
        DM1 --> DM2
        DM2 -->|"No"| DM3
        DM2 -->|"Yes"| DM4
        DM4 --> DM5 --> DM6
    end

    %% ═══════════════════════════════════════════════
    %% 第四部分：硬件驱动层 (Driver Layer)
    %% ═══════════════════════════════════════════════

    subgraph DMA_DRIVER ["🟩 DMA 底层驱动层 (以 DW AXI DMAC 为例)"]
        direction TB
        DRV1["📄 drivers/dmac/dmac_dw_axi.c:154-185<br/><b>dw_axi_dmac_transfer_m2m（src, dst, size, priv）</b><br/>═══════════════════════════════<br/>解析 blockTS = (size >> dma_width) - 1<br/>调用 <b>dw_dmac_mem_to_mem</b> 进行寄存器配置"]
        
        DRV2["📄 drivers/dmac/dmac_dw_axi.c:50-145<br/><b>dw_dmac_mem_to_mem（） — 寄存器级编程</b><br/>═══════════════════════════════<br/>① 轮询复位状态：<br/>   while ((readl(DMAC_AXI0_COMMON_RST_REG) & 1) != 0);<br/>② 轮询通道空闲：<br/>   while ((readl(DMAC_AXI0_COMMON_CH_EN) & 1) != 0);<br/>③ 配置 CH1_CFG / CH1_CFG_HIGH（连续块传输、流量控制、优先级等）<br/>④ 配置源地址和目的地址：<br/>   writeq(DMAC_AXI0_CH1_SAR, src_addr);<br/>   writeq(DMAC_AXI0_CH1_DAR, des_addr);<br/>⑤ 配置传输块大小：<br/>   writel(DMAC_AXI0_CH1_BLOCK_TS, blockTS);<br/>⑥ 配置 CH1_CTL（地址自增、传输位宽、burst size）<br/>⑦ <b>使能通道</b>：<br/>   writel(DMAC_AXI0_COMMON_CH_EN, ... | 0x101);<br/>   <i>（此时硬件 DMA 开始在总线上搬运数据）</i>"]
        
        DRV3["<b>同步等待硬件完成</b><br/>═══════════════════════════════<br/><b>wait_for_event(&dw_axi_dmac→done, wake_expr)</b><br/>📌 阻塞当前进程，让出 CPU 给调度器<br/>📌 当 done == 1 时才被唤醒继续执行"]
        
        DRV1 --> DRV2 --> DRV3
    end

    %% ═══════════════════════════════════════════════
    %% 第五部分：硬件中断与唤醒 (Hardware & IRQ)
    %% ═══════════════════════════════════════════════

    subgraph HW_IRQ ["⚡ 硬件完成与中断处理"]
        direction TB
        IRQ1["🔧 <b>硬件 DMA 控制器</b><br/>═══════════════════<br/>完成数据搬运后，向 CPU 发出中断信号"]
        
        IRQ2["📄 drivers/dmac/dmac_dw_axi.c:42-48<br/><b>dw_dmac_irq_handler（void *data）</b><br/>═══════════════════════════<br/>① wait_for_dmac_complete() 轮询 INTR_STATUS<br/>② 清除中断：writel(DMAC_AXI0_CH1_INTR_CLEAR, 0x3);<br/>③ 发送完成信号：<br/>   <b>__smp_store_release(&dw_axi_dmac→done, 1);</b>"]
        
        IRQ3["<b>唤醒进程</b><br/>═══════════════════<br/>唤醒阻塞在 wait_for_event 上的进程<br/>返回 0 (成功) → 返回到 dma_transfer → 返回到 dma_test_handler"]
        
        IRQ1 --> IRQ2 --> IRQ3
    end

    %% ═══════════════════════════════════════════════
    %% 连接关系
    %% ═══════════════════════════════════════════════
    
    APP2 -->|"memcpy_hw"| DC1
    DC2 -.->|"分配 IOVA"| DM1
    DC3 -->|"调用 transfer_m2m"| DRV1
    DRV3 -.->|"等待中断唤醒"| IRQ3
    IRQ3 -.->|"返回结果"| APP3
```

## DMA 关键机制解析

1. **抽象层解耦 (`dmac_ops`)**：
   - `core/dmac/dmac.c` 维护了全局 DMA 链表 (`LIST_HEAD(dmacs)`)。应用程序使用 `memcpy_hw` 时，只需指定驱动名称（如 `"DMAC0"`），核心层即可完成匹配。
   - 底层驱动（DW AXI DMAC 或 PCI DMA Engine）只需要实现并注册 `dmac_ops { .transfer_m2m = ... }`，将软硬件解耦。

2. **虚拟化映射与 IOMMU 透传 (`dma_mapping`)**：
   - DMA 子系统支持 IOMMU。在申请内存搬运时，通过统一入口 `dma_mapping` 进行虚拟地址映射。
   - `iova_alloc` 维护了虚拟地址分配状态（使用有序链表基于 First-Fit 算法找空闲块）。
   - 代码精妙之处：如果硬件没有开启 IOMMU，`dma_mapping` 会**优雅回退（Fallback）**直接返回传入的物理地址，从而无需改动驱动代码。

3. **同步与异步处理**：
   - 寄存器配置好并启动 DMA 后，CPU **不执行空转死等（busy wait）**，而是通过 `wait_for_event` 将当前进程休眠并让出 CPU。
   - 硬件 DMA 拷贝完成后触发 IRQ 中断，在 ISR 中断处理函数 `dw_dmac_irq_handler` 中将标志位 `done` 置 1。
   - 等待队列检测到条件达成，重新唤醒发起传输的进程，实现零拷贝过程中的 CPU 高效利用。
