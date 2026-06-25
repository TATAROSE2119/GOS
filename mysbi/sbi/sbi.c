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

#include <print.h>
#include <asm/csr.h>
#include <asm/trap.h>
#include <sbi_uart.h>
#include <string.h>
#include <device.h>
#include <sbi.h>
#include "sbi_clint.h"
#include "sbi_uart.h"
#include "sbi_imsic.h"
#include "sbi_trap.h"
#include "autoconf.h"
#include "hpmdriver.h"


static struct sbi_trap_hw_context *h_context[8] = { 0 };
extern void exception_vector(void);
static int pmp_insert_range(unsigned long start, unsigned long end, unsigned long attr);
static do_ext_irq_t do_ext_irq = 0;
static do_timer_irq_t do_timer_irq = 0;
static do_soft_irq_t do_soft_irq = 0;

typedef struct {
	unsigned long start;
	unsigned long end;
	char attr; /* for pmpcfg*/
} pmp_region_t;


void sbi_panic()
{
	sbi_print("sbi panic...\n");

	while (1) ;
}

static void sbi_trap_error(struct sbi_trap_regs *regs, const char *msg, int rc)
{
	sbi_print("mhartid:%d:\n", read_csr(mhartid));
	sbi_print("%s: %s (error %d)\n", __FUNCTION__, msg, rc);
	sbi_print("mcause: 0x%lx  mtval: 0x%lx \n",
		  read_csr(mcause), read_csr(mtval));
	sbi_print("stval: 0x%lx \n", read_csr(CSR_VSTVAL));

	sbi_print("mepc: 0x%lx mstatus : 0x%lx\n", regs->mepc, regs->mstatus);
	sbi_print(" gp : 0x%lx tp : 0x%lx t0 : 0x%lx\n", regs->gp, regs->tp,
		  regs->t0);
	sbi_print(" t1 : 0x%lx t2 : 0x%lx t3 : 0x%lx\n", regs->t1, regs->t2,
		  regs->s0);
	sbi_print(" s1 : 0x%lx a0 : 0x%lx a1 : 0x%lx\n", regs->s1, regs->a0,
		  regs->a1);
	sbi_print(" a2 : 0x%lx a3 : 0x%lx a4 : 0x%lx\n", regs->a2, regs->a3,
		  regs->a4);
	sbi_print(" a5 : 0x%lx a6 : 0x%lx a7 : 0x%lx\n", regs->a5, regs->a6,
		  regs->a7);
	sbi_print(" s2 : 0x%lx s3 : 0x%lx s4 : 0x%lx\n", regs->s2, regs->s3,
		  regs->s4);
	sbi_print(" s5 : 0x%lx s6 : 0x%lx s7 : 0x%lx\n", regs->s5, regs->s6,
		  regs->s7);
	sbi_print(" s8 : 0x%lx s9 : 0x%lx s10: 0x%lx\n", regs->s8, regs->s9,
		  regs->s10);
	sbi_print(" s11: 0x%lx t3 : 0x%lx t4: 0x%lx\n", regs->s11, regs->t3,
		  regs->t4);
	sbi_print(" t5 : 0x%lx t6 : 0x%lx sp: 0x%lx\n", regs->t5, regs->t6,
		  regs->sp);
	sbi_print(" ra: 0x%lx\n", regs->ra);

	sbi_panic();
}

static int sbi_hart_start(unsigned int hartid, unsigned long jump_addr)
{
	struct sbi_trap_hw_context *ctx;

	ctx = h_context[hartid];
	if (!ctx) {
		sbi_print("%s -- invalid hartid, hartid:%d\n", __FUNCTION__,
			  hartid);
		return -1;
	}

	ctx->next_addr = jump_addr;
	strcpy(ctx->next_mode, "S_MODE");
	ctx->wait_var = 1;

	return 0;
}

static void hpm_test()
{
    print("Hello, XiangShan!\n");
    printu_csr(marchid);
    printx_csr(mcountinhibit);
    printx_csr(mcounteren);
    printx_csr(scounteren);

    printu_csr(mcycle);
    printu_csr(minstret);

    se_cc_single(3, MODE_M, Frontend_frontendFlush);
    se_cc_single(11, MODE_M, CtrlBlock_decoder_waitInstr);
    se_cc_double(19, MODE_M, OPTYPE_ADD, MemBlock_loadpipe0_load_req, MemBlock_loadpipe1_load_req);

    // === tmp workload ===
    volatile uint64_t a = 0;
    for(uint64_t i = 0; i < 1000; i++) {
        a += a + i;
    }
    print("%lu\n",a);

    print_event(3);
    print_counter(3);
    print_event(11);
    print_counter(11);
    print_event(19);
    print_counter(19);
}

static int sbi_ecall_handle(unsigned int id, struct sbi_trap_regs *regs)
{
	unsigned long ret_value = 0;
	int ret = 0;
	struct sbi_trap_hw_context *ctx = h_context[read_csr(mhartid)];

	switch (id) {
	case SBI_SET_TIMER:
		clint_timer_event_start(ctx, regs->a0);
		ret = 0;
		break;
	case SBI_CONSOLE_PUTCHAR:
		sbi_uart_putc(regs->a0);
		ret = 0;
		break;
	case SBI_GET_CPU_CYCLE:
		ret_value = read_csr(mcycle);
		break;
	case SBI_GET_CPU_MINSTRET:
		ret_value = read_csr(minstret);
		break;
	case SBI_GET_CPU_ID:
		ret_value = read_csr(mhartid);
		break;
	case SBI_HART_START:
		ret_value = sbi_hart_start(regs->a0, regs->a1);
		break;
	case SBI_SET_MCOUNTEREN:
		write_csr(CSR_MCOUNTEREN, regs->a0);
		break;
	case SBI_GET_CPU_MCOUNTEREN:
		ret_value = read_csr(CSR_MCOUNTEREN);
		break;
#if CONFIG_SELECT_AIA
	case SBI_GET_M_MSI_DATA:
		ret_value = sbi_imsic_alloc_irqs(regs->a0, ctx);
		break;
	case SBI_GET_M_MSI_ADDR:
		ret_value = sbi_imsic_get_mmio(ctx);
		break;
	case SBI_GET_M_MSI_DATA_IPI:
		ctx = h_context[regs->a0];
		ret_value = sbi_imsic_alloc_irqs(regs->a0, ctx);
		break;
	case SBI_GET_M_MSI_ADDR_IPI:
		ctx = h_context[regs->a0];
		ret_value = sbi_imsic_get_mmio(ctx);
		break;
#endif
	case SBI_SET_CSR_MIE:
		write_csr(mie, regs->a0);
		break;
	case SBI_GET_CSR_MIE:
		ret_value = read_csr(mie);
		break;
	case SBI_GET_CSR_MENVCFG:
		ret_value = read_csr(menvcfg);
		break;
	case SBI_SET_MEDELEG:
		write_csr(medeleg, regs->a0);
		break;
	case SBI_GET_MEDELEG:
		ret_value = read_csr(medeleg);
		break;
	case SBI_HPM_TEST:
		hpm_test();
		break;
	case SBI_GET_SMSTATEEN:
		ret_value = read_csr(CSR_MSTATEEN0);
		break;
	case SBI_SET_SMSTATEEN:
		write_csr(CSR_MSTATEEN0, regs->a0);
		break;
	case SBI_PMP_INSERT_RANGE:
		ret_value = pmp_insert_range(regs->a0, regs->a1, regs->a2);
		break;
	}

	regs->a0 = ret_value;
	regs->mepc += 4;

	return ret;
}

static void sbi_ext_process()
{
	if (do_ext_irq)
		do_ext_irq();
	//csr_clear(mie, MIP_MEIP);
	//csr_set(mip, MIP_SEIP);
}

static void sbi_soft_process(void)
{
	struct sbi_trap_hw_context *ctx;
	int hartid = read_csr(mhartid);

	if (!do_soft_irq)
		return;

	ctx = h_context[hartid];

	do_soft_irq(ctx);
}

static void sbi_timer_process(void)
{
	struct sbi_trap_hw_context *ctx;
	int hartid = read_csr(mhartid);

	if (!do_timer_irq)
		return;

	ctx = h_context[hartid];

	do_timer_irq(ctx);
}

void sbi_register_ipi_irq_handler(do_soft_irq_t fn)
{
	if (do_soft_irq)
		return;

	do_soft_irq = fn;
}

void sbi_register_timer_irq_handler(do_timer_irq_t fn)
{
	if (do_timer_irq)
		return;

	do_timer_irq = fn;
}

void sbi_register_ext_irq_handler(do_ext_irq_t fn)
{
	do_ext_irq = fn;
}

void sbi_trap_handler(struct sbi_trap_regs *regs)
{
	int rc = -1;
	unsigned long mcause = read_csr(mcause);
	unsigned long ecall_id = regs->a7;
	const char *msg = "unsupported mcause";
	//sbi_print("%s -- %s:%d mcause:0x%x\n", __FILE__, __FUNCTION__, __LINE__, mcause);

	if (mcause & MCAUSE_IRQ) {
		mcause &= ~MCAUSE_IRQ;

		switch (mcause) {
		case IRQ_M_TIMER:
			sbi_timer_process();
			break;

		case IRQ_M_EXT:
			sbi_ext_process();
			break;

		case IRQ_M_SOFT:
			sbi_soft_process();
			break;

		default:
			msg = "unhandled external interrupt";
			goto trap_error;
		}

		return;
	}

	switch (mcause) {
	case CAUSE_SUPERVISOR_ECALL:
		rc = sbi_ecall_handle(ecall_id, regs);
		msg = "ecall handler failed";
		break;
//	case CAUSE_LOAD_ACCESS:
//	case CAUSE_STORE_ACCESS:
//		msg = "load store access failed";
//		break;
	case CAUSE_FETCH_ACCESS:
		msg = "Instruction access fault";
		break;
	case CAUSE_MISALIGNED_STORE:
		msg = "address misaligned st fault";
		break;
	case CAUSE_MISALIGNED_FETCH:
		msg = "address misaligned ld fault";
		break;
	default:
		break;
	}

trap_error:
	if (rc) {
		sbi_trap_error(regs, msg, rc);
	}
}

static void sbi_trap_init(void)
{
	write_csr(mtvec, exception_vector);
	sbi_print("mtvec=0x%x, 0x%x\n", read_csr(mtvec), exception_vector);

	write_csr(mie, 0);
}

static void sbi_setup_pmp(void)
{
	// Set up a PMP to permit access to all of memory.
	// Ignore the illegal-instruction trap if PMPs aren't supported.
	unsigned long pmpc = PMP_A_NAPOT | PMP_R | PMP_W | PMP_X;
	asm volatile ("la t0, 1f\n\t"
				"csrrw t0, mtvec, t0\n\t"
				"csrw pmpaddr0, %1\n\t"
				"csrw pmpcfg0, %0\n\t"
				".align 2\n\t"
				"1: csrw mtvec, t0"::"r" (pmpc), "r"(-1UL):"t0");
}

/* read  pmpcfg0 */
static void read_pmpcfg(char cfg[64])
{
	int max_pmpcfg = 1;
	for (int i = 0; i < max_pmpcfg ; i++) {
		unsigned long v;
		switch(i) {
			case 0: asm volatile ("csrr %0, %1" : "=r"(v) : "i"(CSR_PMPCFG0)); break;
		}
		for (int j = 0;j < 8; j++)
			cfg[i*8 + j] = (v >> (j*8)) & 0xff;
	}
}

/* write pmpcfg0 */
static void write_pmpcfg(char cfg[64])
{
	int max_pmpcfg = 1;
	for (int i = 0; i < max_pmpcfg; i++) {
		uint64_t v = 0;
		for (int j = 0; j < 8; j++)
			v |= (uint64_t)cfg[i*8 + j] << (j*8);
		switch(i) {
			case 0: write_csr(CSR_PMPCFG0, v);break;
		}
	}
}

/* read pmpaddr0-7 */
static void read_pmpaddr(unsigned long addr[64])
{
	int max_pmpaddr = 8;
	for (int i = 0; i < max_pmpaddr; i++) {
		switch(i) {
			case 0 :
				addr[i] = read_csr(CSR_PMPADDR0);
				break;
			case 1 :
				addr[i] = read_csr(CSR_PMPADDR1);
				break;
			case 2 :
				addr[i] = read_csr(CSR_PMPADDR2);
				break;
			case 3 :
				addr[i] = read_csr(CSR_PMPADDR3);
				break;
			case 4 :
				addr[i] = read_csr(CSR_PMPADDR4);
				break;
			case 5 :
				addr[i] = read_csr(CSR_PMPADDR5);
				break;
			case 6 :
				addr[i] = read_csr(CSR_PMPADDR6);
				break;
			case 7 :
				addr[i] = read_csr(CSR_PMPADDR7);
				break;
			default:				break;
		}
	}
}

/* write pmpaddr0-7 */
static void write_pmpaddr(unsigned long addr[64])
{
	int max_pmpaddr = 8;
	for (int i = 0; i < max_pmpaddr; i++) {
		switch(i) {
			case 0 :
				write_csr(CSR_PMPADDR0, addr[i]);
				break;
			case 1 :
				write_csr(CSR_PMPADDR1, addr[i]);
				break;
			case 2 :
				write_csr(CSR_PMPADDR2, addr[i]);
				break;
			case 3 :
				write_csr(CSR_PMPADDR3, addr[i]);
				break;
			case 4 :
				write_csr(CSR_PMPADDR4, addr[i]);
				break;
			case 5 :
				write_csr(CSR_PMPADDR5, addr[i]);
				break;
			case 6 :
				write_csr(CSR_PMPADDR6, addr[i]);
				break;
			case 7 :
				write_csr(CSR_PMPADDR7, addr[i]);
				break;
		}
	}
}

/* get pmpcfg info and origin pmpaddr info*/
static int pmp_decode(pmp_region_t *out, int max)
{
	char cfg[64];
	unsigned long addr[64];
	read_pmpcfg(cfg);
	read_pmpaddr(addr);

	int nr = 0;
	unsigned long base = 0;
	for (int i = 0; i < 8 && nr < max; i++) {
		char a = (cfg[i] >> 3) & 0x3;
	   if (a == 0) { /* OFF */
			continue;
		}
		unsigned long limit;
		if (addr[i] == ~(unsigned long)0) {
			limit = ~(unsigned long)0;
		} else { // TOR: treat all choices as TOR 
			limit = addr[i] << 2;
		} 

		out[nr].start = base;
		out[nr].end = limit;
		out[nr].attr = cfg[i] & 0xff;
		nr++;
		base = limit;
	}
	return nr;
}

/* reconstruct the pmpcfg with TOR, return used count */
static int pmp_encode(const pmp_region_t *in, int cnt, char cfg[64], unsigned long addr[64])
{
	int used;
	unsigned long base = 0;
	for (int i = 0; i < cnt && used < 64; i++) {
		unsigned long start = in[i].start;
		unsigned long end = in[i].end;
		if (start != base) {
			cfg[used] = 0x0;;
			addr[used] = 0;
			used++;
			base = start;
			if (used >= 64) break;
		}

		cfg[used] = (in[i].attr & 0x7) | PMP_A_TOR;
		addr[used] = end >> 2;

		used++;
		base = end;
	}
	/* close other entries */
	for (int i = used; i < 64; i++) {
		cfg[i] = 0x0;
		addr[i] = 0;
	}
	return used;
}

/* insert new_attr for addr[start,end] */
static int pmp_insert_range(unsigned long start, unsigned long end, unsigned long attr)
{
	char new_attr = (char)(attr & 0xff);
	if (start >= end)
		return -1;
	pmp_region_t tmp[128];
	int nr = pmp_decode(tmp, 8);

	pmp_region_t *out = tmp + 64;
	int on = 0;

	for (int i = 0; i < nr; i++) {
		unsigned long s = tmp[i].start;
		unsigned long e = tmp[i].end;
		char a = tmp[i].attr;

		/* at left side of new area */
		if (e <= start) {
			out[on++] = tmp[i];
			continue;
		}
		/* at right side of new area */
		if (s >= end) {
			out[on++] = tmp[i];
			continue;
		}
		/* overlap with new area, need to be split */
		if (s < start) {
			out[on].start = s;
			out[on].end = start;
			out[on].attr = a;
			on++;
		}
		unsigned long seg_s = (s > start) ? s : start;
		unsigned long seg_e = (e < end) ? e : end;
		out[on].start = seg_s;
		out[on].end = seg_e;
		out[on].attr = new_attr;
		on++;
		if (e > end) {
			out[on].start = end;
			out[on].end = e;
			out[on].attr = a;
			on++;
		}
	}

	/* no original data */
	if (on == 0) {
		if (start > 0) {
			out[on].start = 0;
			out[on].attr = start;
			out[on].attr = PMP_A_TOR | PMP_R | PMP_W | PMP_X;
			on++;
		}
		out[on].start = start;
		out[on].end = end;
		out[on].attr = new_attr;
		on++;
		out[on].start = end;
		out[on].end = ~(unsigned long)0;
		out[on].attr = PMP_A_TOR | PMP_R | PMP_W | PMP_X;
		on++;
	}

	//sort the pmp data
	for (int i = 0; i < on; i++) {
		for (int j = i+1; j < on; j++) {
			if (out[j].start < out[i].start) {
				pmp_region_t t = out[i];
				out[i] = out[j];
				out[j] = t;
			}
		}
	}

	pmp_region_t final[64];
	int fn = 0;

	/* merge adjacent data with the same attr */
	for (int i = 0; i < on; i++) {
		if (fn > 0 &&
				final[fn-1].end == out[i].start &&
				final[fn-1].attr == out[i].attr) {
			final[fn-1].end = out[i].end;
		} else {
			final[fn++] = out[i];
			if (fn >= 64)
				return -3;
		}
	}

	char ncfg[64];
	unsigned long naddr[64];
	pmp_encode(final, fn, ncfg, naddr);
	write_csr(pmpcfg0, 0);
	write_pmpaddr(naddr);
	write_pmpcfg(ncfg);

	return 0;
}

static void sbi_set_next(struct sbi_trap_hw_context *ctx)
{
	extern unsigned long __payload_start;
	unsigned long payload_start = (unsigned long)&__payload_start;

#ifndef PAYLOAD_RUN_IN_M_MODE
	ctx->next_addr = payload_start;
	strcpy(ctx->next_mode, "S_MODE");
#else
	ctx->next_addr = payload_start;
	strcpy(ctx->next_mode, "M_MODE");
#endif

	sbi_print("%s -- next_mode: %s, next_addr:0x%x\n", __FUNCTION__,
		  ctx->next_mode, ctx->next_addr);
}

static inline int next_is_s_mode(char *MODE)
{
	return !strncmp(MODE, "S_MODE", 16);
}

static inline int next_is_m_mode(char *MODE)
{
	return !strncmp(MODE, "M_MODE", 16);
}

static void sbi_jump_to_supervisor(unsigned int hart_id, unsigned long hw_info,
				   unsigned long addr, unsigned long option)
{
	unsigned long val;

	val = read_csr(mstatus);
	val = INSERT_FIELD(val, MSTATUS_MPP, PRV_S);
	val = INSERT_FIELD(val, MSTATUS_MPIE, 0);
	write_csr(mstatus, val);

	write_csr(mepc, addr);
	write_csr(stvec, addr);
	write_csr(satp, 0);
	//write_csr(sie, 0);

	//sbi_print("Ready to jump to S mode. hart_id:%d next_addr:0x%lx boot_optoin:0x%lx\n",
	//	    hart_id, addr, option);

	register unsigned long a0 asm("a0") = hart_id;
	register unsigned long a1 asm("a1") = hw_info;
	register unsigned long a2 asm("a2") = option;
	asm volatile ("mret"::"r" (a0), "r"(a1), "r"(a2));
}

static void sbi_jump_to_machine(unsigned long addr)
{
	typedef void (*fn_t)(void);
	fn_t fn;

	fn = (fn_t) addr;

	sbi_print("%s %d\n", __FUNCTION__, __LINE__);
	fn();
}

void sbi_jump_to_next(struct sbi_trap_hw_context *ctx)
{
	if (next_is_s_mode(ctx->next_mode)) {
        sbi_jump_to_supervisor(ctx->hart_id, ctx->hw_info,
				       ctx->next_addr, ctx->boot_option);
	} else if (next_is_m_mode(ctx->next_mode)) {
		sbi_jump_to_machine(ctx->next_addr);
	} else
		sbi_print("unsupported mode.\n");
}

static void sbi_get_boot_option(struct sbi_trap_hw_context *ctx)
{
	extern unsigned long __boot_option_start;

	ctx->boot_option = (unsigned long)&__boot_option_start;
}

static void sbi_get_hw_info(struct sbi_trap_hw_context *ctx)
{
	extern unsigned long DEVICE_INIT_TABLE, DEVICE_INIT_TABLE_END;
	int nr =
	    (struct device_init_entry *)&DEVICE_INIT_TABLE_END -
	    (struct device_init_entry *)&DEVICE_INIT_TABLE;
	struct device_init_entry *device_entry;
	int n = 0;

	nr -= 1;
	sbi_print("device_info: 0x%x\n",
		  (struct device_init_entry *)&DEVICE_INIT_TABLE);
	for (device_entry = (struct device_init_entry *)&DEVICE_INIT_TABLE; nr;
	     device_entry++, nr--) {
		sbi_print("device %d\n", n++);
		sbi_print("    compatible: %s\n", device_entry->compatible);
		sbi_print("    base_address: 0x%x, len:0x%x\n",
			  device_entry->start, device_entry->len);
		sbi_print("    hwirq: %d\n", device_entry->irq);
		sbi_print("    priv_data: 0x%x\n", device_entry->data);
	}

	ctx->hw_info = (unsigned long)&DEVICE_INIT_TABLE;
}

void delegate_traps(void)
{
	unsigned long interrupts;
	unsigned long exceptions;

	interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;
	exceptions =
		(1UL << CAUSE_BREAKPOINT) |
		(1UL << CAUSE_MISALIGNED_LOAD) | (1UL << CAUSE_MISALIGNED_STORE) |
		(1UL << CAUSE_MISALIGNED_FETCH) | (1UL << CAUSE_FETCH_PAGE_FAULT) |
		(1UL << CAUSE_LOAD_PAGE_FAULT) | (1UL << CAUSE_STORE_PAGE_FAULT) |
		(1UL << CAUSE_USER_ECALL) | (1UL << CAUSE_LOAD_ACCESS) |
		(1UL << CAUSE_STORE_ACCESS) | (1UL << CAUSE_ILLEGAL_INSTRUCTION) |
		(1UL << CAUSE_VIRTUAL_SUPERVISOR_ECALL) | (1UL << CAUSE_LOAD_GUEST_PAGE_FAULT) |
		(1UL << CAUSE_STORE_GUEST_PAGE_FAULT) | (1UL << CAUSE_FETCH_GUEST_PAGE_FAULT) |
		(1UL << CAUSE_VIRTUAL_INST_FAULT);

	write_csr(mideleg, interrupts);
	write_csr(medeleg, exceptions);
}

static void sbi_smstateen_init(void)
{
	unsigned long smstateen = read_csr(CSR_MSTATEEN0);
	smstateen |= (1UL << SMSTATEEN0_HSENVCFG_SHIFT)|(1UL << SMSTATEEN_STATEN_SHIFT);
#if CONFIG_SELECT_AIA
	smstateen |= (1UL << SMSTATEEN0_AIA_SHIFT) |
		     (1UL << SMSTATEEN0_IMSIC_SHIFT) |
		     (1UL << SMSTATEEN0_SVSLCT_SHIFT);
#endif
	write_csr(CSR_MSTATEEN0, smstateen);
	print("sbi_smstateen_init smstateen:0x%x\n", read_csr(CSR_MSTATEEN0));
}

void sbi_exit(unsigned int hart_id)
{
	sbi_print("sbi exit... hart id:%d\n", hart_id);

	while (1) ;
}

void sbi_secondary_init(unsigned int hart_id, struct sbi_trap_hw_context *ctx)
{
	extern unsigned long DEVICE_INIT_TABLE;

	sbi_print("sbi secondary init... hart_id: %d ctx:0x%x\n", hart_id, ctx);

	h_context[hart_id] = ctx;
	ctx->wait_var = 0;
	ctx->hart_id = hart_id;
	ctx->hw_info = (unsigned long)&DEVICE_INIT_TABLE;

	sbi_trap_init();
	sbi_setup_pmp();

	sbi_clint_init(hart_id, ctx);

	write_csr(mie, MIP_MSIP | MIP_MEIP | MIP_MTIP);
	delegate_traps();
}

void sbi_init(unsigned int hart_id, struct sbi_trap_hw_context *ctx)
{
	h_context[hart_id] = ctx;

	if (-1 == sbi_uart_init(hart_id, ctx))
		return;

	sbi_print("complier time : %s\n", BUILD_TIME);
	sbi_print("sbi init... hartid: %d, ctx:%x\n", hart_id, ctx);

	sbi_trap_init();
	sbi_setup_pmp();
	
    if (-1 == sbi_clint_init(hart_id, ctx))
		sbi_print("sbi clint init failed.\n");
#if CONFIG_SELECT_AIA
	if (-1 == sbi_imsic_init(hart_id, ctx))
		sbi_print("sbi imsic init failed.\n");
#endif
	sbi_set_next(ctx);
	sbi_get_hw_info(ctx);
	sbi_get_boot_option(ctx);

	write_csr(mie, MIP_MSIP | MIP_MEIP | MIP_MTIP);

	delegate_traps();

	//enable SE0(bit 63), ENVCFG(bit 62) in MSTATEEN0, otherwise VCPU_RUN will fail
	sbi_smstateen_init();
}
