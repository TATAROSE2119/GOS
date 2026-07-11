#include "cache_flush.h"
#include "asm/barrier.h"

//RISCV中一个 CBOM的缓存块大小是64字节
#define RISCV_CBOM_BLOCK_SIZE 64UL  

/*
 * align_down - 将地址向下对齐到 RISCV_CBOM_BLOCK_SIZE 边界
 * @addr: 需要对齐的地址
 * @size: 对齐粒度（未使用，统一使用 RISCV_CBOM_BLOCK_SIZE）
 *
 * 返回不大于 addr 的最大对齐地址。
 */
static inline unsigned long align_down(unsigned long addr)
{
	return addr & ~(RISCV_CBOM_BLOCK_SIZE -1);
}

/*
 * align_up - 将地址向上对齐到 RISCV_CBOM_BLOCK_SIZE 边界
 * @addr: 需要对齐的地址
 * @size: 对齐粒度（未使用，统一使用 RISCV_CBOM_BLOCK_SIZE）
 *
 * 返回不小于 addr 的最小对齐地址。
 */
static inline unsigned long align_up(unsigned long addr)
{
	return (addr + RISCV_CBOM_BLOCK_SIZE -1)& ~(RISCV_CBOM_BLOCK_SIZE-1);
}

/*
 * dcache_clean_range - 对指定地址范围执行 dcache clean+flush（写回并失效）
 * @base: 起始虚拟地址
 * @size: 范围大小（字节）
 *
 * 为什么需要对齐？
 * RISC-V CMO（Cache Management Operation）指令（如 cbo.clean、cbo.flush、
 * cbo.inval）以缓存块（Cache Block，通常 64 字节）为粒度进行操作。硬件要求
 * 传给这些指令的地址必须对齐到缓存块边界，否则会触发地址不对齐异常。
 *
 * 因此，必须将用户传入的任意地址范围 [base, base+size) 向外扩展到覆盖
 * 该范围的、对齐到缓存块边界的地址区间：
 * - 起始地址向下对齐（align_down）：确保从包含 base 的那个缓存块开始操作，
 *   不会漏掉 base 所在缓存块的前半部分脏数据。
 * - 结束地址向上对齐（align_up）：确保覆盖到包含 base+size 的那个缓存块，
 *   不会漏掉尾部跨越缓存块边界的数据。
 *
 * 如果不做对齐而直接传入未对齐地址，轻则漏掉边界缓存块导致数据不一致，重则
 * 触发 CPU 异常。对齐是正确使用 CMO 指令的前提。
 */
void dcache_clean_range(unsigned long base, unsigned long size)
{
	unsigned long end;
	unsigned long addr;

	if (!size) {
		return;
	}
	end = align_up(base + size);
	addr = align_down(base);

    for (; addr<end; addr+=RISCV_CBOM_BLOCK_SIZE) {
        cbo_cache_clean(addr);
    }
    mb();

}
void dcache_flush_range(unsigned long base, unsigned long size){
    unsigned long addr;
    unsigned long end;
    if (!size) {
        return;
    }
	end = align_up(base + size);
	addr = align_down(base);

    for (; addr<end; addr+=RISCV_CBOM_BLOCK_SIZE) {
        cbo_cache_flush(addr);
    }
    mb();
}
void dcache_inval_range(unsigned long base, unsigned long size){
    unsigned long addr;
    unsigned long end;
    if (!size) {
        return;
    }
	end = align_up(base + size);
	addr = align_down(base);

    for (; addr<end; addr+=RISCV_CBOM_BLOCK_SIZE) {
        cbo_cache_inval(addr);
    }
    mb();
}
  