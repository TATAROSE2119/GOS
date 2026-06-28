#include "gos.h"
#include <asm/type.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <mm.h>
#include <print.h>

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
		      unsigned long va_base, unsigned int shift, unsigned long start,
		      unsigned long end)
{
	int i;
	for (i = 0; i < 512; i++) {
		unsigned long src_pte = src_tbl[i];
		unsigned long va =
		    va_base |
		    ((unsigned long)i << shift); // 把索引 i 放到当前级的位域上

		if (src_pte == 0) {
			continue;
		}
		if (va >= end || va + (1UL << shift) <= start) {
			continue;
		}
		if (shift == PAGE_SHIFT) {
			copy_leaf(&dst_tbl[i], &src_tbl[i], va);
		} else if (pmd_leaf(src_pte)) {
			print("copy_page_range: skip huge page @0x%lx\n", va);
			continue;
		} else {
			unsigned long *src_child = (unsigned long *)phy_to_virt(
			    pfn_to_phys(pte_pfn(src_pte)));
			unsigned long *dst_child;

			if (dst_tbl[i] == 0) {
				unsigned long pa = alloc_zero_page(0);
				if (!pa) {
					return -1;
				}
				dst_tbl[i] = (pa >> PAGE_SHIFT)
						 << _PAGE_PFN_SHIFT |
					     _PAGE_PRESENT;
			}
			dst_child = (unsigned long *)phy_to_virt(
			    pfn_to_phys(pte_pfn(dst_tbl[i])));
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

 int copy_page_range(unsigned long start,unsigned long end,unsigned long *dst_pgdp, unsigned long *src_pgdp){
    int ret=copy_level(dst_pgdp, src_pgdp, 0, PGDIR_SHIFT, start, end);
    if (ret) {
        return ret;
    }
    /* 父 PTE 被改成只读+COW，旧 TLB 项必须失效 */
    local_flush_tlb_range(start, end - start, PAGE_SIZE);
    return 0;
 }
#endif
