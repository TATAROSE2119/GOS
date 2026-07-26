#include "gos.h"
#include <asm/type.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <mm.h>
#include <print.h>
#include "string.h"
#include "task.h"

#if CONFIG_COW

#define COW_SHARE_END 0x1000UL

// 处理一个4KB 叶子页表，父子共享物理页，并按区间决定共享还是COW
static void copy_leaf(unsigned long *dst_ptep, unsigned long *src_ptep,
		      unsigned long va)
{
	unsigned long pte = *src_ptep;
	unsigned long pa = pfn_to_phys(pte_pfn(pte));

	if (va < COW_SHARE_END) {
		/* 共享内存页：子映射与父完全相同，保持可写 */
		*dst_ptep = pte;
	} else {
		/* 可写匿名页：父子都置只读 + COW */
		unsigned long cow = pte_mkcow(pte_wrprotect(pte));
		*src_ptep = cow; /* 父也要变，否则父写不触发缺页 */
		*dst_ptep = cow;
	}
	get_page(pa);
}

/* lockstep 递归：同时走父(src)、子(dst)同一级页表 */
static int copy_level(unsigned long *dst_tbl, unsigned long *src_tbl,
		      unsigned long va_base, unsigned int shift,
		      unsigned long start, unsigned long end)
{ // shift表示当前页表级别的位移量，start和end表示要复制的父用户区的虚拟地址范围，L2的shift=30，L1的shift=21，L0的shift=12
	int i;
	for (i = 0; i < 512; i++) {	// 遍历当前页表级别的所有页表项，512表示每个页表有512个条目
		unsigned long src_pte = src_tbl[i];
		unsigned long va =
		    va_base |
		    ((unsigned long)i << shift); // 把索引 i 放到当前级的位域上

		if (src_pte == 0) {
			continue;
		}
		if (va >= end ||
		    va + (1UL << shift) <=
			start) { // 超出父区间，跳过，检查当前页表项覆盖的虚拟地址范围是否在父区间内
			continue;
		}
		if (shift ==
		    PAGE_SHIFT) { // 这个分支表示当前页表级别是叶子页表（4KB
				  // 页）
			copy_leaf(
			    &dst_tbl[i], &src_tbl[i],
			    va); // 叶子页表，调用 copy_leaf 处理父子共享物理页
		} else if (
		    pmd_leaf(
			src_pte)) { // 超页叶子，父子共享物理页，按区间决定共享还是COW
			print("copy_page_range: skip huge page @0x%lx\n", va);
			continue;
		} else { // 非叶子页表，递归处理下级页表
			unsigned long *src_child = (unsigned long *)phy_to_virt(
			    pfn_to_phys(pte_pfn(src_pte)));// 父下级页表的虚拟地址
			unsigned long *dst_child;// 子下级页表的虚拟地址

			if (dst_tbl[i] == 0) {		
				/* 子无此级表，新建空表 */
				unsigned long pa = alloc_zero_page(0);
				if (!pa) {
					return -1;
				}
				dst_tbl[i] = (pa >> PAGE_SHIFT)
						 << _PAGE_PFN_SHIFT |
					     _PAGE_PRESENT;	// 子页表项指向新建的空表
			} else if (dst_tbl[i] == src_pte) {	// 子该项与父指向同一张下级表(memcpy 内核 PGD 继承而来)。直接写会连带改到父/内核共享的表。克隆一份(复制整表以保留内核等兄弟项)，令子拥有私有下级表。
				/*
				 * 子该项与父指向同一张下级表(memcpy 内核 PGD
				 * 继承
				 * 而来)。直接写会连带改到父/内核共享的表。克隆一份
				 * (复制整表以保留内核等兄弟项)，令子拥有私有下级表。
				 */
				unsigned long pa = alloc_zero_page(0);	// 分配一页物理内存作为子下级页表
				if (!pa) {
					return -1;
				}
				memcpy((void *)phy_to_virt(pa),
				       (void *)src_child, PAGE_SIZE);
				dst_tbl[i] = (pa >> PAGE_SHIFT)
						 << _PAGE_PFN_SHIFT |
					     _PAGE_PRESENT;
			}
			/* else: 子已有私有表(前一 region 克隆)，复用 */
			dst_child = (unsigned long *)phy_to_virt(pfn_to_phys(pte_pfn(dst_tbl[i])));
			if (copy_level(dst_child, src_child, va, shift - 9,
				       start, end)) {
				return -1;
			}
		}
	}
	return 0;
}
/*
 * 把父用户区 [start,end) 复制到子：共享物理页、父子只读+COW、get_page。
 * dst_pgdp/src_pgdp 为两棵 PGD 的【虚拟地址】指针。
 */

int copy_page_range(unsigned long start, unsigned long end,
		    unsigned long *dst_pgdp, unsigned long *src_pgdp)
{
	int ret = copy_level(dst_pgdp, src_pgdp, 0, PGDIR_SHIFT, start, end);
	if (ret) {
		return ret;
	}
	/* 父 PTE 被改成只读+COW，旧 TLB 项必须失效 */
	local_flush_tlb_range(start, end - start, PAGE_SIZE);
	return 0;
}

int cow_handle_write(unsigned long addr, unsigned long *ptep)
{
	unsigned long pte = *ptep;
	unsigned long old_pa = pfn_to_phys(pte_pfn(pte));	// 获取旧的物理页地址
	struct task *task = get_current_task();	// 获取当前任务
	unsigned long asid = task ? (unsigned long)task->id : 0;	// 获取当前任务的 ASID（Address Space Identifier），用于区分不同任务的地址空间

	if (page_count(old_pa) == 1) {	// 物理页独占，直接把 PTE 改成可写即可
		// 独占叶，无需拷贝
		set_pte(ptep, pte_uncow_mkwrite(pte));	// 把 PTE 改成可写
	} else { // 共享页	page_count(old_pa) >= 2，必须拷贝一份新的物理页给当前任务	
		unsigned long new_va = (unsigned long)mm_alloc(PAGE_SIZE);	// 分配一页新的虚拟地址空间
		unsigned long new_pa, newpte;	// 新的物理页地址和新的 PTE

		if (!new_va) {
			return -1;
		}
		memcpy((void *)new_va, (void *)phy_to_virt(old_pa), PAGE_SIZE);		// 把旧的物理页内容拷贝到新的虚拟地址空间
		new_pa = virt_to_phy(new_va);	// 获取新的物理页地址

		newpte = (pte & ~_PAGE_PFN_MASK) |
			 ((new_pa >> PAGE_SHIFT) << _PAGE_PFN_SHIFT);	// 构造新的 PTE，把新的物理页地址放到 PTE 中
		set_pte(ptep, pte_uncow_mkwrite(newpte));	// 把 PTE 改成新的物理页地址，并设置为可写

		put_page(old_pa);	// 旧的物理页引用计数减 1，如果引用计数为 0，则释放该物理页
	}
	local_flush_tlb_page_asid(addr, asid);	// 失效当前任务的 TLB 中对应虚拟地址的页表项
	return 0;
}

/* 在 pgdp(虚拟指针) 上把 va 走到 4K 叶子 PTE；未映射返回 NULL */
static unsigned long *walk_to_leaf(unsigned long *pgdp, unsigned long va)
{
	unsigned long *tbl = pgdp;
	unsigned int shift = PGDIR_SHIFT;

	while (1) {
		unsigned long *ptep = &tbl[(va >> shift) & 0x1FF];

		if (shift == PAGE_SHIFT)
			return ptep; /* 4K 叶子 */
		if (*ptep == 0)
			return NULL; /* 中间项缺失 */
		if (pmd_leaf(*ptep))
			return ptep; /* 超页叶子 */
		tbl = (unsigned long *)phy_to_virt(pfn_to_phys(pte_pfn(*ptep)));
		shift -= 9;
	}
}

/*
 * U 态 store 缺页入口：若是 COW 写保护缺页则处理并返回 0，否则返回 -1。
 * 供 do_user_exception 调用。
 */
int cow_try_handle_store(unsigned long addr)
{
	unsigned long *pgdp = (unsigned long *)phy_to_virt(get_current_pgd());
	unsigned long *ptep = walk_to_leaf(pgdp, addr);

	if (ptep && pte_is_valid(*ptep) && pte_is_cow(*ptep) &&
	    !(*ptep & _PAGE_WRITE))
		return cow_handle_write(addr, ptep);

	return -1;
}

#endif // CONFIG_COW
