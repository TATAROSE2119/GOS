#include "gos.h"
#include <asm/type.h>
#include <asm/pgtable.h>
#include <mm.h>
#include <print.h>

#if CONFIG_COW




#endif

#define COW_SHARE_END 0x1000UL


//处理一个4KB 叶子页表，父子共享物理页，并按区间决定共享还是COW
static void copy_leaf(unsigned long *dst_ptep,unsigned long *src_ptep,unsigned long va){
    unsigned long pte=*src_ptep;
    unsigned long pa=pfn_to_phys(pte_pfn(pte));

    if (va < COW_SHARE_END) {
        *dst_ptep=pte;
    }
}