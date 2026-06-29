/*
 * Copyright (c) 2024 Beijing Institute of Open Source Chip (BOSC)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "align.h"
// #include "asm/pgtable.h"
#include "devicetree.h"
#include "gos.h"
#include "spinlocks.h"
#include "tiny_mm.h"
#include "vmap.h"
#include <asm/type.h>
#include <device.h>
#include <mm.h>
#include <print.h>
#include <string.h>

extern int mmu_is_on;
extern unsigned long bss_end;
extern unsigned long va_pa_offset;

static spinlock_t mem_lock = __SPINLOCK_INITIALIZER;

static struct memory_block mm_blocks = {0};

extern char dtb_bin[];

unsigned long get_phy_start(void) { return mm_blocks.memory_block_start[0]; }

unsigned long get_phy_end(void)
{
	int last = mm_blocks.avail - 1;

	return (mm_blocks.memory_block_size[last] +
		mm_blocks.memory_block_start[last]);
}

void memory_block_add(unsigned long base, unsigned long size)
{
	unsigned long i = 0;
#if CONFIG_SELECT_4K_DIRECT_MAPPING
	unsigned long ddr_start = PAGE_ALIGN((unsigned long)(&bss_end));
#elif CONFIG_SELECT_2M_DIRECT_MAPPING
	unsigned long ddr_start = PAGE_ALIGN_2M((unsigned long)(&bss_end));
#elif CONFIG_SELECT_1G_DIRECT_MAPPING
	unsigned long ddr_start = PAGE_ALIGN_1G((unsigned long)(&bss_end));
#endif

	if (mm_blocks.avail >= MAX_MEM_BLOCK_COUNT)
		return;

	if (base + size < ddr_start)
		return;

	if (base + size < ddr_start)
		return;

	if (base < ddr_start) {
		size -= ddr_start - base;
		base = ddr_start;
	}

	while (i < size) {
		mm_blocks.memory_block_start[mm_blocks.avail] = base + i;
		if (size - i > MAX_BYTE_PER_MAPS)
			mm_blocks.memory_block_size[mm_blocks.avail] =
			    MAX_BYTE_PER_MAPS;
		else
			mm_blocks.memory_block_size[mm_blocks.avail] = size - i;
		print("block size:0x%lx\n",
		      mm_blocks.memory_block_size[mm_blocks.avail]);
		mm_blocks.maps[mm_blocks.avail].map_nr =
		    mm_blocks.memory_block_size[mm_blocks.avail] / PAGE_SIZE;
		mm_blocks.avail++;

		i += MAX_BYTE_PER_MAPS;
	}
}

static void mm_reserved(unsigned long base, unsigned long size)
{
	int i, n;

	n = mm_blocks.avail;
	for (i = 0; i < n; i++) {
		unsigned long block_start;
		unsigned int block_size;
		unsigned long mem_map;
		int index, nr;
		int page_nr;
		struct mem_maps *mem_maps = &mm_blocks.maps[i];
		int per_mem_map = sizeof(mem_maps->maps[0]) * 8;

		page_nr = size % PAGE_SIZE == 0 ? size / PAGE_SIZE
						: size / PAGE_SIZE + 1;

		block_start = mm_blocks.memory_block_start[i];
		block_size = mm_blocks.memory_block_size[i];

		if (!(base >= block_start &&
		      (base + size <= block_start + block_size))) {
			if (size != 0)
				print("Invalid Reserved Memory params... "
				      "Please check dts..\n");
			continue;
		}

		index = (base - block_start) / PAGE_SIZE;
		for (nr = 0; nr < page_nr; nr++, index++) {
			mem_map = mem_maps->maps[(index / per_mem_map)];
			mem_map |= (1UL << (index % per_mem_map));
			mem_maps->maps[(index / per_mem_map)] = mem_map;
		}

		mm_blocks.memory_block_resv_start[mm_blocks.reserved_cnt] =
		    base;
		mm_blocks.memory_block_resv_size[mm_blocks.reserved_cnt] =
		    page_nr * PAGE_SIZE;
		mm_blocks.reserved_cnt++;
	}
}

void mm_init(struct device_init_entry *hw)
{
	int i, n;
	struct mem_maps *maps;

	mm_blocks.avail = 0;
	mm_blocks.max_byte_per_maps = MAX_BYTE_PER_MAPS;

	dtb_scan_memory((void *)dtb_bin, memory_block_add);

	n = mm_blocks.avail;
	for (i = 0; i < n; i++) {
		unsigned long start;
		unsigned int size;
		unsigned long tmp = 0;
		unsigned long nr_free_pages = 0;

		start = mm_blocks.memory_block_start[i];
		size = mm_blocks.memory_block_size[i];

		tmp = start;
		while (tmp < start + size) {
			nr_free_pages++;
			tmp += PAGE_SIZE;
		}
		maps = &mm_blocks.maps[i];

		memset((char *)maps->maps, 0, maps->map_nr / 8);

		print("Available Memory: phy_start_address:0x%lx, "
		      "phy_end_address:0x%lx, available size:%dKB, %d "
		      "available pages, page_size:%d\n",
		      mm_blocks.memory_block_start[i],
		      mm_blocks.memory_block_start[i] +
			  mm_blocks.memory_block_size[i],
		      mm_blocks.memory_block_size[i] / 1024, nr_free_pages,
		      PAGE_SIZE);
	}

	mm_blocks.reserved_cnt = 0;
	dtb_scan_reserved_memory((void *)dtb_bin, mm_reserved);

	print("Reserved Memory info --\n");
	for (i = 0; i < mm_blocks.reserved_cnt; i++)
		print("  start:0x%lx, size:0x%lx\n",
		      mm_blocks.memory_block_resv_start[i],
		      mm_blocks.memory_block_resv_size[i]);

#if CONFIG_TINY
	tiny_init();
#endif
}

void *mm_alloc_align(unsigned long align, unsigned int size)
{
	int n, i;
	int page_nr = N_PAGE(size);
	struct mem_maps *mem_maps;
	unsigned long index;
	unsigned long mem_map;
	unsigned long align_addr;
	int per_mem_map;
	void *ret;
	irq_flags_t flags;

	if (align <= PAGE_SIZE)
		return mm_alloc(size);

	align = align / PAGE_SIZE * PAGE_SIZE;

	n = mm_blocks.avail;

	spin_lock_irqsave(&mem_lock, flags);
	for (i = 0; i < n; i++) {
		unsigned long start;
		unsigned int len;
		int total;
		int nr = 0;

		start = mm_blocks.memory_block_start[i];
		len = mm_blocks.memory_block_size[i];
		align_addr = ALIGN_SIZE(start, align);

		if (!(align_addr >= start && align_addr < start + len))
			continue;

		if (!(align_addr + size >= start && align_addr < start + len))
			continue;

		mem_maps = &mm_blocks.maps[i];
		per_mem_map = sizeof(mem_maps->maps[0]) * 8;
		total = mem_maps->map_nr;
		index = ((unsigned long)align_addr - start) / PAGE_SIZE;
		if (index >= total)
			continue;

		while (index < total) {
			mem_map = mem_maps->maps[(index / per_mem_map)];
			if (((mem_map >> (index % per_mem_map)) & (1UL)) == 0) {
				if (++nr == page_nr)
					goto success;
			} else {
				nr = 0;
				align_addr += align;
				index = ((unsigned long)align_addr - start) /
					PAGE_SIZE;
				continue;
			}

			index++;
		}
	}
	spin_unlock_irqrestore(&mem_lock, flags);
	print("%s -- out of memory!!\n", __FUNCTION__);

	return NULL;

success:
	for (index = index + 1 - page_nr; page_nr; index++, page_nr--) {
		per_mem_map = sizeof(mem_maps->maps[0]) * 8;
		mem_map = mem_maps->maps[(index / per_mem_map)];
		mem_map |= (1UL << (index % per_mem_map));
		mem_maps->maps[(index / per_mem_map)] = mem_map;
	}
	spin_unlock_irqrestore(&mem_lock, flags);

	if (!mmu_is_on)
		ret = (void *)align_addr;
	else
		ret = (void *)((unsigned long)align_addr + va_pa_offset);

	return ret;
}

void *__mm_alloc(unsigned int size)
{
	int page_nr = N_PAGE(size), n, i;
	int index = 0, nr = 0;
	void *ret;
	struct mem_maps *mem_maps;
	unsigned long mem_map;
	int per_mem_map;
	void *addr;
	irq_flags_t flags;

	n = mm_blocks.avail;
	for (i = 0; i < n; i++) {
		int total;
		void *start;

		index = 0;
		nr = 0;
		mem_maps = &mm_blocks.maps[i];

		total = mem_maps->map_nr;
		per_mem_map = sizeof(mem_maps->maps[0]) * 8;
		addr = (void *)mm_blocks.memory_block_start[i];
		start = addr;
		/*
		 * Find free pages from mem_maps according to page_nr
		 * index/per_mem_map indicates that the page represented by
		 * index is located in which mem_map of mem_maps
		 * index%per_mem_map indicates that the page represented by
		 * index is locate in which bit of its mem_map If can find
		 * page_nr continuous bits in mem_maps, goto success and the
		 * addr is the start address of according continues this bits.
		 */
		spin_lock_irqsave(&mem_lock, flags);
		while (index < total) {
			mem_map = mem_maps->maps[(index / per_mem_map)];
			if (((mem_map >> (index % per_mem_map)) & (1UL)) == 0) {
				if (++nr == page_nr)
					goto success;
			} else {
				addr += (nr + 1) * PAGE_SIZE;
				index = ((unsigned long)addr -
					 (unsigned long)start) /
					PAGE_SIZE;
				nr = 0;
				continue;
			}

			index++;
		}

		spin_unlock_irqrestore(&mem_lock, flags);
	}
	print("out of memory!!\n");

	return NULL;

success:
	/*
	 * Set founded page_nr continues bits to 1 in mem_maps
	 */
	for (index = index + 1 - page_nr; page_nr; index++, page_nr--) {
		per_mem_map = sizeof(mem_maps->maps[0]) * 8;
		mem_map = mem_maps->maps[(index / per_mem_map)];
		mem_map |= (1UL << (index % per_mem_map));
		mem_maps->maps[(index / per_mem_map)] = mem_map;
	}

	spin_unlock_irqrestore(&mem_lock, flags);

	if (!mmu_is_on)
		ret = addr;
	else
		ret = (void *)((unsigned long)addr + va_pa_offset);

	return ret;
}

void *mm_alloc_fix(unsigned long addr, unsigned int size)
{
	int page_nr = N_PAGE(size), n, total, i, tmp_nr;
	void *ret;
	unsigned long phy_address_start;
	struct mem_maps *mem_maps;
	unsigned long index, tmp_i;
	unsigned long mem_map;
	int per_mem_map;
	irq_flags_t flags;

	n = mm_blocks.avail;
	spin_lock_irqsave(&mem_lock, flags);
	for (i = 0; i < n; i++) {
		unsigned long start;
		unsigned int len;

		start = mm_blocks.memory_block_start[i];
		len = mm_blocks.memory_block_size[i];

		if (!((unsigned long)addr >= start &&
		      (unsigned long)addr < start + len))
			continue;

		mem_maps = &mm_blocks.maps[i];
		per_mem_map = sizeof(mem_maps->maps[0]) * 8;
		total = mem_maps->map_nr;

		phy_address_start = start;
		goto check_and_alloc;
	}

	spin_unlock_irqrestore(&mem_lock, flags);
	return NULL;

check_and_alloc:
	index = ((unsigned long)addr - phy_address_start) / PAGE_SIZE;
	if (index >= total) {
		spin_unlock_irqrestore(&mem_lock, flags);
		return NULL;
	}

	tmp_i = index;
	tmp_nr = page_nr;
	for (; tmp_nr; tmp_nr--, tmp_i++) {
		mem_map = mem_maps->maps[(tmp_i / per_mem_map)];
		if (((mem_map >> (tmp_i % per_mem_map)) & (1UL)) != 0) {
			spin_unlock_irqrestore(&mem_lock, flags);
			return NULL;
		}
	}
	tmp_i = index;
	tmp_nr = page_nr;
	for (; tmp_nr; tmp_nr--, tmp_i++) {
		mem_map = mem_maps->maps[(tmp_i / per_mem_map)];
		mem_map |= (1UL << (tmp_i % per_mem_map));
		mem_maps->maps[(tmp_i / per_mem_map)] = mem_map;
	}
	spin_unlock_irqrestore(&mem_lock, flags);

	if (mmu_is_on)
		ret = (void *)phy_to_virt(addr);
	else
		ret = (void *)addr;

	return ret;
}

void *mm_alloc(unsigned int size)
{
#if CONFIG_TINY
	if (mmu_is_on) {
		if (size <= PAGE_SIZE / 2)
			return tiny_alloc(size);
	}
#endif
	return __mm_alloc(size);
}

struct memory_block *get_mm_blocks() { return &mm_blocks; }

#if CONFIG_COW
static struct page *page_map;  // 引用计数的大表，虚拟地址的指针
static unsigned long pfn_base; // 最低物理页号
static unsigned long pfn_nr;   // 表里一共多少页

/**
 * pa_2_page - 物理地址 -> struct page 元数据指针
 * @pa: 物理地址（Physical Address）
 *
 * 将物理地址转换为对应 struct page 的指针。内部做 PFN 偏移计算及
 * 越界检查：若 page_map 未初始化、pfn 低于 pfn_base 或超出范围，
 * 返回 NULL。
 *
 * 返回值: 指向对应物理页元数据的指针，失败返回 NULL
 * 调用上下文: 任意上下文（纯计算，无锁无阻塞）
 *
 * 用法:
 *   struct page *p = pa_2_page(0x80000000);
 *   if (p) { p->refcount++; }
 */
static struct page *pa_2_page(unsigned long pa)
{
	unsigned long pfn = pa >> PAGE_SHIFT;
	if (!page_map || pfn < pfn_base || pfn - pfn_base >= pfn_nr) {
		return NULL;
	}
	return &page_map[pfn - pfn_base];
}

/**
 * page_refcount_init - 初始化物理页引用计数子系统
 *
 * 根据系统可用物理内存范围（get_phy_start() ~ get_phy_end()），
 * 通过 __mm_alloc() 分配一块连续物理内存作为 page_map 数组，
 * 用于存储所有物理页的 struct page 元数据（当前仅 refcount 字段）。
 * 分配后 memset 清零。
 *
 * 调用上下文: 内核初始化阶段，mm_init() 之后，开 MMU 之前或之后均可
 *
 * 用法:
 *   // 在 main() 初始化序列中调用一次
 *   page_refcount_init();
 */
void page_refcount_init(void)
{
	unsigned long phy_start = get_phy_start();
	unsigned long phy_end = get_phy_end();
	unsigned long bytes;

	pfn_base = phy_start >> PAGE_SHIFT;
	pfn_nr = (phy_end - phy_start) >> PAGE_SHIFT;
	bytes = pfn_nr * sizeof(struct page);

	page_map = __mm_alloc(bytes);
	if (!page_map) {
		print("page_refcount_init: alloc failed,%ld pages\n", pfn_nr);
		return;
	}
	memset((void *)page_map, 0, bytes);
	print("page_refcount: %ld pages [0x%lx,0x%lx) table %ld KB \n", pfn_nr,
	      phy_start, phy_end, bytes / 1024);
}

// ============================================================
// 引用计数三原语：get_page / page_count / put_page
//
// refcount 语义:
//   0  ->  页面未激活 或 已完全释放（可回收至 bitmap）
//   1  ->  （保留，实际不使用）
//   2  ->  基引用（页面存活）+ 0 个外部引用（首次 get_page 后）
//   N  ->  基引用 + (N-2) 个外部引用
//
// 生命周期:
//   __mm_alloc()        -> refcount = 0（memset 后的初始态）
//   get_page()          -> refcount = 2（激活 + 首次持有）
//   get_page() 再调用   -> refcount++
//   put_page()          -> refcount--
//   ... 反复 ...
//   put_page() 到 0     -> 调用者负责归还内存到 __mm_free()
// ============================================================

/**
 * get_page - 增加物理页的引用计数
 * @pa: 物理地址
 *
 * 对该物理页增加一次引用。若该页 refcount 为 0（刚分配未激活），
 * 则直接置为 2（基引用 + 当前引用）；否则递增。
 *
 * 调用上下文: 任意进程上下文，内部持 mem_lock 自旋锁
 * 副作用:    修改 page_map 中对应页的 refcount
 *
 * 用法:
 *   // 首次引用刚分配的页
 *   void *addr = __mm_alloc(PAGE_SIZE);
 *   get_page(virt_to_phy(addr));   // refcount: 0 -> 2
 *
 *   // fork 时共享父进程页面
 *   get_page(parent_pa);            // refcount: 2 -> 3
 */
void get_page(unsigned long pa)
{
	struct page *p;
	irq_flags_t flags;

	spin_lock_irqsave(&mem_lock, flags);
	p = pa_2_page(pa);
	if (p) {
		if (p->refcount == 0) {
			p->refcount = 2;
		} else {
			p->refcount++;
		}
	}

	spin_unlock_irqrestore(&mem_lock, flags);
}

/**
 * page_count - 查询物理页的当前引用计数
 * @pa: 物理地址
 *
 * 返回该物理页的 refcount 值。若 pa 不在可用范围或 refcount==0，
 * 返回 1（保守值，防止误判为可释放）。
 *
 * 返回值: 引用计数值（>= 1）
 * 调用上下文: 任意上下文，内部持 mem_lock 自旋锁
 *
 * 用法:
 *   if (page_count(pa) == 2) {
 *       // 仅当前进程在使用，可直接写（无需 COW 复制）
 *       do_direct_write(pa);
 *   } else {
 *       // 多个进程共享，需要 COW 复制新页
 *       do_cow_copy(pa);
 *   }
 */
int page_count(unsigned long pa)
{
	struct page *p;
	irq_flags_t flags;
	int n;

	spin_lock_irqsave(&mem_lock, flags);
	p = pa_2_page(pa);
	n = (p && p->refcount) ? p->refcount : 1;
	spin_unlock_irqrestore(&mem_lock, flags);
	return n;
}

/**
 * __put_page_locked - 减少引用计数（内部版本，调用者已持锁）
 * @pa: 物理地址
 *
 * 对物理页减少一次引用。根据 refcount 状态：
 *   refcount <= 1  ->  清零，返回 1（页面可回收）
 *   refcount == 2  ->  清零，返回 0（基引用释放，但无外部引用残留）
 *   refcount >  2  ->  递减，返回 0（尚有其他引用者）
 *
 * 返回值: 1 = 页面可回收，0 = 仍有引用或不需要回收
 *
 * 注意: 调用者必须已持有 mem_lock。返回 1 时调用者负责
 *       将页面归还给 __mm_free()。
 */
static int __put_page_locked(unsigned long pa)
{
	struct page *p = pa_2_page(pa);

	if (!p) {
		return 1;
	}
	if (p->refcount <= 1) {
		p->refcount = 0;
		return 1;
	}
	if (p->refcount == 2) {
		p->refcount = 0;
		return 0;
	}
	p->refcount--;
	return 0;
}

/**
 * put_page - 减少物理页的引用计数（外部接口）
 * @pa: 物理地址
 *
 * 对物理页减少一次引用。内部获取 mem_lock 后调用 __put_page_locked()。
 *
 * 返回值: 1 = 页面可回收，0 = 仍有引用
 * 调用上下文: 任意进程上下文，内部持 mem_lock 自旋锁
 *
 * 用法:
 *   if (put_page(pa)) {
 *       // refcount 归零，可以释放物理页
 *       __mm_free(phy_to_virt(pa), PAGE_SIZE);
 *   }
 */
int put_page(unsigned long pa)
{
	int ret;
	irq_flags_t flags;

	spin_lock_irqsave(&mem_lock, flags);
	ret = __put_page_locked(pa);
	spin_unlock_irqrestore(&mem_lock, flags);
	return ret;
}
#else
static inline int __put_page_locked(unsigned long pa)
{
	(void)pa;
	return 1;
}

#endif

void __mm_free(void *addr, unsigned int size)
{
	int page_nr = N_PAGE(size), n, total, i;
	unsigned long phy_address_start;
	struct mem_maps *mem_maps;
	unsigned long index;
	unsigned long mem_map;
	int per_mem_map;
	irq_flags_t flags;

	if (mmu_is_on)
		addr = addr - va_pa_offset;

	n = mm_blocks.avail;

	for (i = 0; i < n; i++) {
		unsigned long start;
		unsigned int len;

		start = mm_blocks.memory_block_start[i];
		len = mm_blocks.memory_block_size[i];

		if (!((unsigned long)addr >= start &&
		      (unsigned long)addr < start + len))
			continue;

		mem_maps = &mm_blocks.maps[i];
		per_mem_map = sizeof(mem_maps->maps[0]) * 8;
		total = mem_maps->map_nr;

		phy_address_start = start;
		goto release;
	}

	return;

release:
	index = ((unsigned long)addr - phy_address_start) / PAGE_SIZE;
	if (index >= total)
		return;

	spin_lock_irqsave(&mem_lock, flags);
	/* set bits in mem_maps according to [addr, addr + size) to 0 */
	for (; page_nr; page_nr--, index++) {

		unsigned long pa = phy_address_start + index * PAGE_SIZE;
		if (!__put_page_locked(pa)) {
			continue;
		}

		mem_map = mem_maps->maps[(index / per_mem_map)];
		mem_map &= ~(unsigned long)((1UL) << (index % per_mem_map));
		mem_maps->maps[(index / per_mem_map)] = mem_map;
	}
	spin_unlock_irqrestore(&mem_lock, flags);
}

void mm_free(void *addr, unsigned int size)
{
#if CONFIG_TINY
	if (mmu_is_on) {
		if (size <= PAGE_SIZE / 2) {
			tiny_free(addr);
			return;
		}
	}
#endif
	__mm_free(addr, size);
}

void reserved_mem_walk(void (*fn)(unsigned long addr, unsigned int nr,
				  void *data),
		       void *data)
{
	int i;

	for (i = 0; i < mm_blocks.reserved_cnt; i++) {
		fn(mm_blocks.memory_block_resv_start[i],
		   mm_blocks.memory_block_resv_size[i], data);
	}
}

void unused_mem_walk(void (*fn)(unsigned long addr, unsigned int nr,
				void *data),
		     void *data)
{
	int n, i;
	irq_flags_t flags;
	unsigned long mem_end;

	n = mm_blocks.avail;
	mem_end = get_phy_end();
	spin_lock_irqsave(&mem_lock, flags);
	for (i = 0; i < n; i++) {
		int total, nr = 0, index = 0;
		struct mem_maps *mem_maps;
		unsigned long mem_map;
		int per_mem_map;
		void *addr, *addr_walk;

		mem_maps = &mm_blocks.maps[i];

		total = mem_maps->map_nr;
		per_mem_map = sizeof(mem_maps->maps[0]) * 8;
		addr = addr_walk = (void *)mm_blocks.memory_block_start[i];

		while (index <= total) {
			mem_map = mem_maps->maps[(index / per_mem_map)];
			if ((((mem_map >> (index % per_mem_map)) & (1UL)) ==
			     0) &&
			    ((unsigned long)addr_walk < mem_end)) {
				nr++;
			} else {
				if (nr != 0)
					fn((unsigned long)addr, PAGE_SIZE * nr,
					   data);
				nr = 0;
				addr += (nr + 1) * PAGE_SIZE;
			}

			addr_walk += PAGE_SIZE;
			index++;
		}
	}
	spin_unlock_irqrestore(&mem_lock, flags);
}

static void mem_range_contain(unsigned long addr, unsigned int size, void *data)
{
	struct mem_range_info {
		unsigned long addr;
		unsigned int size;
		int contain;
	};
	struct mem_range_info *mem = (struct mem_range_info *)data;

	if ((mem->addr >= addr) && ((mem->addr + mem->size) <= (addr + size)))
		mem->contain = 1;
}

int mem_range_is_free(unsigned long addr, unsigned int size)
{
	struct {
		unsigned long addr;
		unsigned int size;
		int contain;
	} tmp = {0};

	tmp.addr = addr;
	tmp.size = size;
	tmp.contain = 0;

	unused_mem_walk(mem_range_contain, &tmp);

	return tmp.contain;
}

static void print_unused_mem_info(unsigned long addr, unsigned int len,
				  void *data)
{
	print("addr: 0x%lx len: 0x%x\n", addr, len);
}

void walk_unused_mem_and_print(void)
{
	unused_mem_walk(print_unused_mem_info, NULL);
}
